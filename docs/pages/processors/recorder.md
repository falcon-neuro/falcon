---
description: "Falcon's Recorder: writes the samples and events of its connections into one file. Its inputs and options, what is recorded, the file's format and how to read it with Python."
---

# Recorder

{{ api("Recorder") }} writes everything its connections bring, samples and events alike, into one file in the run's directory, for analysis after the run.

Writing to a file can stall for a moment. The recorder should therefore run on a core that isn't realtime, behind [rings](../data_flow/rings.md), where it can't hold up another core.

## Inputs

| Name | Carries |
|------|---------|
| `inputs` | Any number of connections, at least one. Each brings samples, or events of any kind. |

Each connection has a name, by which the file calls it (see [The file](#the-file)). A name holds letters, digits and `_` only, and can't be empty.

The recorder has no output.

## Options

| Option | Meaning | Default |
|--------|---------|---------|
| `file_name` | The name of the file, in the run's directory. It is a name only, without a folder. | `recording.bin` |

The file must not exist yet, so two recorders in one graph need different file names.

## What is recorded

| | Recorded |
|-|----------|
| A sample | Its number and the value of every channel. The times in its header and its number of lost samples are not recorded. |
| An event | Every value it holds. For an event that belongs to a sample, this includes the whole header. |

The recorder gathers 1024 samples of a connection before it writes them, which is 34 ms of samples at 30 kHz. It writes an event when the event arrives.

When the run ends, the recorder writes the samples it still holds and then writes to the log how many records each connection gave. A run that is killed loses the samples that were still gathered, up to 1023 for each connection.

## The file

The file's first line is text in JSON. It names the connections and says how the records of each are laid out. The line of a recording with one connection of samples and one of [`Spikes`](../data_flow/events.md#spikes) events, shown here on several lines:

```json
{"format": "falcon_recording", "version": 2, "slots": [
  {"name": "signal_generator_output", "kind": "samples", "channels": 70, "rate": 30000,
   "record_bytes": 288},
  {"name": "spike_detector_output", "kind": "events", "record_bytes": 48, "fields": [
    {"name": "header.sample_number", "type": "u64", "offset": 0, "bytes": 8, "count": 1},
    {"name": "header.device_timestamp", "type": "u64", "offset": 8, "bytes": 8, "count": 1},
    {"name": "header.arrival_tsc", "type": "u64", "offset": 16, "bytes": 8, "count": 1},
    {"name": "header.lost_sample_count", "type": "u64", "offset": 24, "bytes": 8, "count": 1},
    {"name": "channels", "type": "channel_mask", "offset": 32, "bytes": 16, "count": 70}]}]}
```

Chunks follow the line, up to the end of the file. All numbers in them are little-endian.

| Part of a chunk | Size | Holds |
|-----------------|------|-------|
| Connection | 4 bytes | The connection's place in `slots`, counted from 0, as an unsigned integer. |
| Record count | 4 bytes | The number of records in the chunk, as an unsigned integer. |
| Records | Record count × `record_bytes` | The records, one after another. |

A sample's record is the sample's number as an unsigned 64-bit integer, followed by the channel values in the order of the channels, each a float32. Its `record_bytes` is therefore 8 plus 4 for each channel.

An event's record holds the event's values at the places that `fields` lists:

| Entry of a field | Meaning |
|------------------|---------|
| `name` | The value's name. A value of the header has a name that starts with `header.`. |
| `type` | `bool`, an unsigned integer (`u8`, `u16`, `u32`, `u64`), a signed integer (`i8`, `i16`, `i32`, `i64`), a floating-point number (`f32`, `f64`) or `channel_mask`. |
| `offset` | The byte of the record at which the value starts, counted from 0. |
| `bytes` | The size of the whole value. |
| `count` | The number of values of that type. For a channel mask, the number of channels. |

A channel mask holds one bit per channel, in 64-bit words: channel $n$ is bit $n \bmod 64$ of word $\lfloor n / 64 \rfloor$. A bit that is set means that the event names that channel.

## Reading a recording

This Python program reads a recording with NumPy. It takes the samples of the first connection and the sample numbers of the events of the second:

```python
import json
import struct

import numpy as np

with open("recording.bin", "rb") as file:
    slots = json.loads(file.readline())["slots"]
    chunks = [[] for _ in slots]
    while chunk_header := file.read(8):
        slot_index, record_count = struct.unpack("<II", chunk_header)
        chunks[slot_index].append(file.read(record_count * slots[slot_index]["record_bytes"]))

slot = slots[0]
samples = np.frombuffer(
    b"".join(chunks[0]),
    dtype=[("sample_number", "<u8"), ("values", "<f4", (slot["channels"],))],
)
print(samples["values"].shape)  # (number of samples, number of channels)

slot = slots[1]
field = next(field for field in slot["fields"] if field["name"] == "header.sample_number")
events = np.frombuffer(
    b"".join(chunks[1]),
    dtype=np.dtype(
        {
            "names": ["sample_number"],
            "formats": ["<u8"],
            "offsets": [field["offset"]],
            "itemsize": slot["record_bytes"],
        }
    ),
)
print(events["sample_number"][:5])
```

## Size of a recording

A connection of samples with $C$ channels at the rate $f$ fills

$$
f \cdot (8 + 4 C) \text{ bytes per second}
$$

A stream of 384 channels at 30 kHz, for example, fills 46.3 MB per second, which is 167 GB per hour.

## Lost samples

The recorder writes the samples it receives and doesn't fill a gap. Lost samples therefore show in a recording as a jump in the sample numbers. An event that belongs to a sample also holds the number of samples lost before it.

## Limitations

- The recorder records for the whole run. It can't be started or stopped during a run.
- The file's format has a version number, 2 for the format described here. A later version may differ.
