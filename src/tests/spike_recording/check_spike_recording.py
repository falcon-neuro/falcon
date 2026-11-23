# /// script
# requires-python = ">=3.12"
# dependencies = ["numpy", "matplotlib"]
# ///
"""Checks a file that src/pipelines/spike_recording_pipeline.cpp recorded, against itself: every
recorded spike must sit on a sample where its channel crosses the threshold in the recorded signal,
and every such crossing must have its spike. It judges the whole way a spike takes into the file:
the list of the event's fields in the first line, the event's bytes, the channel mask as the reader
decodes it, and the sample numbers of the two slots.

    uv run src/tests/spike_recording/check_spike_recording.py <the program's folder>/runs/<run>/spike_recording.bin

Build the pipeline and run it for a few seconds first. The threshold and the spike rate below are
the pipeline's: the file doesn't hold them.
"""

import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import plot_recording  # noqa: E402

THRESHOLD = -70.0  # µV, SpikeDetector's in the pipeline
SPIKES_PER_SECOND_AND_CHANNEL = 5  # the frequency of the pipeline's square wave


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    description, records = plot_recording.read_recording(pathlib.Path(sys.argv[1]))
    slots = description["slots"]
    sample_slots = [slot for slot in slots if slot["kind"] == "samples"]
    event_slots = [slot for slot in slots if slot["kind"] == "events"]
    if len(sample_slots) != 1 or len(event_slots) != 1:
        raise SystemExit("expected one slot of samples and one of events")
    sample_slot, event_slot = sample_slots[0], event_slots[0]
    channel_count = sample_slot["channels"]

    failures: list[str] = []

    mask_fields = [
        field for field in event_slot.get("fields", []) if field["type"] == "channel_mask"
    ]
    if len(mask_fields) != 1 or mask_fields[0]["count"] != channel_count:
        raise SystemExit(
            f"slot {event_slot['name']} lists no channel mask of the signal's {channel_count} channels"
        )
    mask_name = mask_fields[0]["name"]

    samples = records[sample_slot["name"]]
    spikes = records[event_slot["name"]]
    if samples.size < 2 or spikes.size == 0:
        raise SystemExit("the recording holds too few samples or no spikes")
    sample_numbers = samples["sample_number"]
    lost_sample_count = int(sample_numbers[-1] - sample_numbers[0]) + 1 - samples.size
    print(f"{samples.size} samples, {lost_sample_count} lost, {spikes.size} spike events")
    if lost_sample_count != 0:
        failures.append(f"{lost_sample_count} samples are missing from the recording")

    # A spike is a channel's first sample below the threshold after one that wasn't.
    values = samples["values"]
    is_crossing = (values[:-1] >= THRESHOLD) & (values[1:] < THRESHOLD)
    crossing_rows, crossing_channels = np.nonzero(is_crossing)
    crossings = set(zip(sample_numbers[crossing_rows + 1].tolist(), crossing_channels.tolist()))

    mask_words = spikes[mask_name]
    is_on_channel = plot_recording.channels_of_mask(mask_words, channel_count)
    spike_rows, spike_channels = np.nonzero(is_on_channel)
    recorded_spikes = set(
        zip(spikes[plot_recording.SAMPLE_NUMBER][spike_rows].tolist(), spike_channels.tolist())
    )
    # The recorder may have stopped between a spike's sample and the spike, at either end.
    first_judged, last_judged = int(sample_numbers[1]), int(sample_numbers[-1])
    judged_spikes = {spike for spike in recorded_spikes if first_judged <= spike[0] <= last_judged}

    spikes_without_crossing = judged_spikes - crossings
    crossings_without_spike = crossings - recorded_spikes
    print(f"{len(crossings)} threshold crossings in the signal, {len(judged_spikes)} spikes on them")
    if spikes_without_crossing:
        failures.append(f"{len(spikes_without_crossing)} spikes are on no threshold crossing")
    if crossings_without_spike:
        failures.append(f"{len(crossings_without_spike)} threshold crossings have no spike")

    all_bits = np.unpackbits(
        np.ascontiguousarray(mask_words).view(np.uint8), axis=1, bitorder="little"
    )
    padding_bit_count = int(all_bits[:, channel_count:].sum())
    if padding_bit_count != 0:
        failures.append(f"{padding_bit_count} mask bits are set beyond the last channel")

    seconds = (last_judged - first_judged) / sample_slot["rate"]
    spikes_per_channel = np.bincount(spike_channels, minlength=channel_count)
    expected_per_channel = SPIKES_PER_SECOND_AND_CHANNEL * seconds
    print(
        f"spikes per channel over {seconds:.2f} s: {spikes_per_channel.min()} to "
        f"{spikes_per_channel.max()}, expected about {expected_per_channel:.1f}"
    )
    if np.any(np.abs(spikes_per_channel - expected_per_channel) > 1):
        failures.append("a channel's spike count is more than 1 off the expected number")

    if failures:
        raise SystemExit("FAILED: " + "; ".join(failures))
    print("passed")


if __name__ == "__main__":
    main()
