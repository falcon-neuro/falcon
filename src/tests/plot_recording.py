# /// script
# requires-python = ">=3.12"
# dependencies = ["numpy", "matplotlib"]
# ///
"""Plots a file that Recorder wrote. The file describes itself: its first line is JSON that names
each connection (a slot) and says how its records are laid out. Chunks follow, each a u32 for the
slot, a u32 for the number of records, then that many records of that slot. A sample's record is its
number as a u64 and its channels' values in order as float32. An event's record is the event as it
was in memory, and the first line lists its fields, each with its name, type, place, size and count:
a field of the event's header is named header.sample_number and so on. All little-endian. A file of
version 1, where an event's record is its header alone, is read too.

    uv run src/tests/plot_recording.py <the program's folder>/runs/<run>/ripple_detector.bin

One sample slot is plotted, a panel per channel: the first one, or the one named with --samples.
Every event slot whose events have a sample number is drawn as vertical lines, a colour per slot. An
event with a mask of the plotted stream's channels, such as a spike, is drawn on those channels
only. With --mean-square, a last panel
shows the mean over the channels of their squared values, which is what RippleDetector compares
with its threshold.
"""

import argparse
import json
import pathlib
import struct

import matplotlib.pyplot as plt
import numpy as np

# An event of a version 1 file, under the names a version 2 file gives the same fields.
HEADER_ONLY_EVENT_TYPE = np.dtype(
    [
        ("header.sample_number", "<u8"),
        ("header.device_timestamp", "<u8"),
        ("header.arrival_tsc", "<u8"),
        ("header.lost_sample_count", "<u8"),
    ]
)
NUMBER_TYPES = {
    "bool": "?",
    "u8": "<u1",
    "u16": "<u2",
    "u32": "<u4",
    "u64": "<u8",
    "i8": "<i1",
    "i16": "<i2",
    "i32": "<i4",
    "i64": "<i8",
    "f32": "<f4",
    "f64": "<f8",
}
SAMPLE_NUMBER = "header.sample_number"
CHUNK_HEADER = struct.Struct("<II")
EVENT_COLOURS = ["tab:red", "tab:purple", "tab:orange", "tab:brown", "tab:pink"]


def event_type_of(slot: dict) -> np.dtype:
    """An event's record, from the fields its slot lists. The bytes between fields are skipped."""
    names, formats, offsets = [], [], []
    for field in slot["fields"]:
        if field["type"] == "channel_mask":
            # Kept as its 64-bit words; channels_of_mask() turns them into one bool per channel.
            field_format = ("<u8", (field["bytes"] // 8,))
        elif field["type"] not in NUMBER_TYPES:
            raise SystemExit(f"slot {slot['name']} has a field of an unknown type, {field['type']}")
        elif field["count"] == 1:
            field_format = NUMBER_TYPES[field["type"]]
        else:
            field_format = (NUMBER_TYPES[field["type"]], (field["count"],))
        names.append(field["name"])
        formats.append(field_format)
        offsets.append(field["offset"])
    return np.dtype(
        {"names": names, "formats": formats, "offsets": offsets, "itemsize": slot["record_bytes"]}
    )


def channels_of_mask(words: np.ndarray, channel_count: int) -> np.ndarray:
    """One row per event, one bool per channel: bit n of word n // 64 is channel n."""
    bits = np.unpackbits(np.ascontiguousarray(words).view(np.uint8), axis=1, bitorder="little")
    return bits[:, :channel_count].astype(bool)


def record_type_of(slot: dict) -> np.dtype:
    if slot["kind"] == "samples":
        record_type = np.dtype([("sample_number", "<u8"), ("values", "<f4", (slot["channels"],))])
    elif slot["kind"] == "events":
        record_type = event_type_of(slot) if "fields" in slot else HEADER_ONLY_EVENT_TYPE
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
    if description.get("format") != "falcon_recording" or description.get("version") not in (1, 2):
        raise SystemExit(f"{path} is not a falcon_recording file of version 1 or 2")

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
        if SAMPLE_NUMBER not in events.dtype.names:
            print(f"{event_slot['name']}: not drawn, since its events have no sample number")
            continue
        event_seconds = (events[SAMPLE_NUMBER].astype(np.int64) - first_sample_number) / rate
        is_event_shown = (event_seconds >= arguments.start) & (event_seconds < window_end)
        shown_event_seconds = event_seconds[is_event_shown]
        # A mask of the plotted stream's channels says on which panels each event is drawn.
        mask_fields = [
            field
            for field in event_slot.get("fields", [])
            if field["type"] == "channel_mask" and field["count"] == channel_count
        ]
        if mask_fields:
            is_on_channel = channels_of_mask(
                events[mask_fields[0]["name"]][is_event_shown], channel_count
            )
        else:
            is_on_channel = np.ones((shown_event_seconds.size, channel_count), dtype=bool)
        print(f"{event_slot['name']}: {shown_event_seconds.size} of its events are in the plot:")
        for event_second, channels_of_event in zip(shown_event_seconds, is_on_channel):
            channel_text = (
                f", channels {np.flatnonzero(channels_of_event).tolist()}" if mask_fields else ""
            )
            print(f"  at {event_second:.6f} s{channel_text}")
        colour = EVENT_COLOURS[event_slot_index % len(EVENT_COLOURS)]
        for channel in range(channel_count):
            for event_second in shown_event_seconds[is_on_channel[:, channel]]:
                panels[channel].axvline(event_second, color=colour, linewidth=0.8)
        for panel in panels[channel_count:]:
            for event_second in shown_event_seconds:
                panel.axvline(event_second, color=colour, linewidth=0.8)
        if shown_event_seconds.size > 0:
            # An empty line, for the legend alone: the first event may not be on the first channel.
            panels[0].plot([], [], color=colour, linewidth=0.8, label=event_slot["name"])
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
