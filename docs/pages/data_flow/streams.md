---
description: "Falcon's streams: what a sample holds, a stream's channel count, rate and unit, and how its channels are processed in groups with SIMD instructions."
---

# Streams

A stream is the series of samples one source produces, one after another. All samples of a stream have the same channels and come at the same rate.

## A sample

| Part | Holds |
|------|-------|
| Values | One value for each channel. Each value is a 32-bit floating-point number (float32). |
| Header | The sample's number, the time the device took it, the time it reached Falcon and the number of samples lost before it. |

## A stream's properties

| Property | Meaning |
|----------|---------|
| Channel count | The number of channels each sample holds. |
| Rate | The number of samples per second, in Hz. It is the rate of the stream's source: Falcon assumes no particular rate. |
| Unit | µV for a stream of voltages. The processor that reads a device converts the device's values to µV, so every processor after it works in µV. |

A stream keeps its channel count and its rate from the processor that starts it to every processor that reads it.

## Channel vectors

Falcon processes the channels of a sample in groups, which is the main reason it is fast. It uses SIMD instructions, with which the CPU does the same operation on several values at once. A spike detector, for example, compares 16 channels with its threshold in one instruction, in the time a comparison of one channel takes.

The channels processed together are a channel vector. Its size is the number of float32 values that one register of the CPU holds: 16 with AVX-512, and 8 with AVX2, which is the least Falcon runs on.

As an example, the channels of three streams packed into channel vectors:

| Channel count | Channel vectors with AVX-512 | Channel vectors with AVX2 |
|---------------|------------------------------|---------------------------|
| 32 | 2 | 4 |
| 64 | 4 | 8 |
| 384 | 24 | 48 |

An operation on a sample takes one instruction for each channel vector. For 384 channels that is 24 instructions with AVX-512, where handling one channel at a time would take 384.

A channel count that is not a multiple of 16 is filled up with empty channels to the next multiple, so that every channel vector is whole. A stream of 100 channels, for example, has 112, of which 12 are empty.

## In the code

A stream's channel count, rate and value type are the struct {{ api("StreamInfo") }}. A sample's header is the struct {{ api("SampleHeader") }}.
