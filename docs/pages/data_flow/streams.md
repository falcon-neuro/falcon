---
description: "Falcon's streams: what a sample holds, a stream's channel count, rate and unit, and how its channels are processed in groups with SIMD instructions."
---

# Streams

A stream is the series of samples one source produces, one after another. All samples of a stream have the same channels and come at the same rate.

## A sample

| Part | Holds |
|------|-------|
| Values | One value for each channel. Each value is a 32-bit floating-point number (float32). |
| Header | The sample's number, the time the device took it, the time it reached Falcon and the number of samples lost before it. It is the struct {{ api("SampleHeader") }}. |

## A stream's properties

| Property | Meaning |
|----------|---------|
| Channel count | The number of channels each sample holds. |
| Rate | The number of samples per second, in Hz. It is set by the stream's source. Falcon does not assume a particular rate. |
| Unit | µV for a stream of voltages. The processor that reads a device converts the device's values to µV, so every processor after it works in µV. |

The channel count and the rate of a stream do not change between the processor that produces it and the processors that read it. Both are held by the struct {{ api("StreamInfo") }}.

## Channel vectors

Falcon processes the channels of a sample in groups, which is the main reason it is fast. It uses SIMD instructions, which apply the same operation to several values at once. A spike detector, for example, compares 16 channels with its threshold in one instruction, in the time it takes to compare one channel.

The channels processed together are a channel vector. Its size is the number of float32 values that one register of the CPU holds: 16 with AVX-512 and 8 with AVX2. AVX2 is the minimum Falcon requires.

As an example, channel counts packed into channel vectors:

| Channel count | Channel vectors with AVX-512 | Channel vectors with AVX2 |
|---------------|------------------------------|---------------------------|
| 32 | 2 | 4 |
| 64 | 4 | 8 |
| 384 | 24 | 48 |

An operation on a sample takes one instruction for each channel vector. For 384 channels that is 24 instructions with AVX-512, instead of 384 when the channels are handled one at a time.

A channel count that is not a multiple of 16 is padded with empty channels up to the next multiple, so that every channel vector is full. A stream of 100 channels, for example, is stored as 112 channels, 12 of them empty.
