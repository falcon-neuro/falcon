---
description: "Falcon's NeuralynxInput: the raw signal of a Neuralynx acquisition system, read from the network and converted to µV. Its output and options, the network connection, the records it accepts and how it numbers the samples and finds lost ones."
---

# NeuralynxInput

{{ api("NeuralynxInput") }} reads the raw data records that a Neuralynx acquisition system sends over the network. A record holds one value for each A/D channel of the system at one time point. The input takes a run of these channels, converts their values to µV and outputs one sample for each record.

The input asks the operating system for a new record on every pass and doesn't wait for one. It therefore needs a core of its own that never sleeps.

## Output

| Name | Carries |
|------|---------|
| `output` | Samples in µV, at the acquisition system's sampling rate. A sample holds the A/D channels from `first_channel` to `last_channel`. |

The processor has no input.

## Options

| Option | Meaning | Default |
|--------|---------|---------|
| `address` | The IP address on this computer at which the records arrive, written as four numbers from 0 to 255 with dots between them, e.g. 192.168.3.100. | 127.0.0.1 |
| `port` | The UDP port at which the records arrive, from 1 to 65535. | 5000 |
| `first_channel` | The first A/D channel of the stream, counted from 0. It can't be negative. | 0 |
| `last_channel` | The last A/D channel of the stream. The stream holds every A/D channel from the first to the last, so the number of these channels must equal the stream's channel count. It can't be before the first. | 127 |
| `sampling_rate` | The acquisition system's sampling rate, in Hz. It differs between systems. It must equal the stream's rate and can't be above 1 MHz. | 32768 |
| `ad_bit_volts` | Neuralynx's ADBitVolts: the voltage that one step of a raw value stands for, in volts (see [Conversion to µV](#conversion-to-v)). It depends on the acquisition hardware. A negative value inverts the signal. It can't be 0. | 0.00000030517578125 |
| `no_data_warning_milliseconds` | The time without a record after which a warning is logged, in milliseconds. It must be above 0. | 100 |

## The network connection

The input listens for UDP datagrams at `address` and `port`. It expects one record in each datagram. Records arrive only if the acquisition system sends them to that address and port.

Requirements:

- A network card of this computer has the address given in `address`.
- No other program listens at the same address and port.

If a requirement isn't met, the run fails at its start with an error message.

At the start of a run the input reports, for each network card of the computer, whether the card collects several packets before it passes them to the CPU. A card that does so delays the records by a varying time, so the report for such a card is a warning.

Records that arrived while the run was starting are discarded on the input's first pass. The stream therefore begins with a record that has just arrived.

When no record has arrived for `no_data_warning_milliseconds`, the input logs a warning. It logs the next such warning only after records have arrived again. The warning also comes when no record has arrived since the start of the run, for example because the address or the port is wrong.

## Records

A record has the format of Neuralynx's raw data files[^format]: a row of 32-bit values.

| Part of a record | Use |
|------------------|-----|
| Start mark, packet type and size | Checked. |
| Timestamp, in µs | Gives the sample's number and shows lost samples (see [Sample numbers and lost samples](#sample-numbers-and-lost-samples)). |
| Status | Not used. |
| Parallel input port | Not used. |
| Ten reserved values | Not used. |
| One value for each A/D channel | The values of the channels from `first_channel` to `last_channel` are converted to µV. |
| Checksum | Checked. |

The input discards a record whose length is not that of a record, that doesn't begin with the start mark 2048, whose packet type isn't 1, whose size value doesn't agree with its length, or whose checksum is wrong. It logs a warning that gives the reason and the number of records discarded so far. When the run ends, it logs the total.

A record that holds fewer A/D channels than `last_channel` requires ends the run with an error message.

[^format]: Neuralynx, Inc. *Neuralynx Data File Formats*, revision 1.1, 2013, section "Raw Data A/D Record".

## Sample numbers and lost samples

A record's timestamp is the time at which the acquisition system took the sample, in whole microseconds. The input numbers the samples from these timestamps.

Let $f_s$ be `sampling_rate`. The first record of a run, with the timestamp $t_0$ in µs, gets the sample number

$$
n_0 = \operatorname{round}\left(\frac{t_0 \, f_s}{10^6}\right)
$$

For every later record the input takes the step $s$, the record's timestamp minus the timestamp of the record before it. A timestamp holds whole microseconds, so two samples in a row are expected to be the sample period, cut to whole microseconds, apart, or one microsecond more. The longest expected step is

$$
L = \left\lfloor \frac{10^6}{f_s} \right\rfloor + 1
$$

which is 31 µs at 32768 Hz and 32 µs at 32000 Hz.

| Step | What the input does |
|------|---------------------|
| 0 or below | The record is repeated or late. The input discards it and logs a warning. |
| From 1 µs to $L$ | The record holds the next sample. Its number is the previous number plus 1. |
| Above $L$ | A gap. The number rises by $\operatorname{round}(s \, f_s / 10^6)$, so the numbers of the lost samples are skipped. The input counts the lost samples and logs a warning. The record's own sample is output. |

With a sampling rate of 32768 Hz:

| Timestamp (µs) | Step (µs) | Sample number | What happens |
|----------------|-----------|---------------|--------------|
| 1,000,000 | | 32,768 | The first record. |
| 1,000,030 | 30 | 32,769 | The next sample. |
| 1,000,061 | 31 | 32,770 | The next sample. |
| 1,000,122 | 61 | 32,772 | A gap of two sample periods. One sample is lost. |
| 1,000,122 | 0 | | The record is discarded. |
| 1,000,153 | 31 | 32,773 | The next sample. |

!!! note "Edge cases"
    - A step just above $L$, e.g. 32 µs at 32768 Hz, is a gap in which no sample is lost. The input logs the warning and gives the sample the next number.
    - After the first record the numbers are counted from the steps. They are not computed from each timestamp, so a system whose rate differs slightly from `sampling_rate` doesn't cause a number to be skipped or repeated over time.

The warning for a gap gives the step, the number of lost samples and the number of the sample after the gap. When the run ends, the input logs the number of samples lost in the whole run and the number of repeated or late records it discarded.

## The samples' headers

| Part of the header | Value |
|--------------------|-------|
| The sample's number | Counted from the records' timestamps, as described above. |
| The time the device took it | The record's timestamp, in µs. |
| The time it reached Falcon | Read when the input has received the record from the operating system. |
| The number of samples lost before it | The samples lost in gaps since the start of the run. |

## Conversion to µV

Let $r_c$ be the raw value of A/D channel $c$, a signed 32-bit integer, and $a$ the value of `ad_bit_volts`. The channel's value in the sample, in µV, is

$$
v_c = 10^6 \, a \, r_c
$$

The product is computed in double precision and then rounded to float32.

ADBitVolts is the name Neuralynx gives to the voltage of one A/D step. Neuralynx states it in volts, so the option takes the value as Neuralynx states it. It must be the value for the raw data that the acquisition system sends.

Falcon can't check the value: with a wrong one, every channel is scaled by the same wrong factor.

## Limitations

- The parallel input port's value in the records is not used, so a graph can't react to the acquisition system's digital inputs.
- The stream's channels are a run of consecutive A/D channels. Channels that are not next to each other can't be selected.
- The input doesn't compare `sampling_rate` with the rate at which records arrive. A value that is too high shows as gap warnings, because the steps are longer than expected. A value that is too low is not noticed.
- If the acquisition system restarts during a run and its timestamps begin again at a lower value, every record is discarded as late until the run is started again.
- The time a record spends in the network card and the operating system before the input reads it is not measured.
