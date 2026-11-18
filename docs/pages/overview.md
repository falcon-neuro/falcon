---
description: "What Falcon is: open-source software that reads neural signals during recording, runs them through a graph of processors and triggers an output."
---

# Overview

Falcon is open-source software for closed-loop neuroscience experiments. It reads neural signals from an acquisition system during recording and passes them through a graph of processors, such as filters and detectors. When the graph detects a pattern of interest, Falcon triggers an output, for example a TTL pulse to a stimulator. The user builds the graph for an experiment from these processors.

Falcon is designed for recordings with hundreds of channels sampled at 30 kHz. It processes them sample by sample on a standard CPU.

A new engine is coming soon. This documentation describes the new version and grows as it is built.

Falcon runs on Linux and is released under the GNU General Public License v3. The source code is on [GitHub](https://github.com/falcon-neuro/falcon).

## Documentation

| Page | Content |
|------|---------|
| [Processor reference](processor_reference.md) | The built-in processors, each with its inputs, outputs and options. |
| [Streams](data_flow/streams.md) | What a stream of samples holds, and how its channels are processed. |
| [Events](data_flow/events.md) | The events the built-in processors emit. |
| [Rings](data_flow/rings.md) | How a connection's data gets from one core to another. |
| [Glossary](glossary.md) | Definitions of the terms used in this documentation. |

Still to come: the pages of the other processors, how Falcon works, the system requirements, installation, a first experiment, example experiments and writing a processor of your own.

## Origins

Falcon was first described by Davide Ciliberti and Fabian Kloosterman:

- Ciliberti D, Kloosterman F (2017). Falcon: a highly flexible open-source software for closed-loop neuroscience. *Journal of Neural Engineering* 14, 045004. <https://doi.org/10.1088/1741-2552/aa7526>

The new engine builds on that work.
