---
description: "What Falcon is: open-source software that reads neural signals during recording, runs them through a graph of processors and triggers an output."
---

# Overview

Falcon is open-source software for closed-loop neuroscience experiments. It reads neural signals from an acquisition system while they are being recorded, runs them through a graph of processors, such as filters and detectors, and triggers an output, for example a TTL pulse to a stimulator, when the pattern of interest appears. Each experiment builds its own graph from these processors.

Falcon is designed for dense recordings, with hundreds of channels sampled at 30 kHz, processed sample by sample on a standard CPU.

Falcon runs on Linux and is released under the GNU General Public License v3. The source code is on [GitHub](https://github.com/falcon-neuro/falcon).

## Documentation

- [Processor reference](processor_reference.md): the built-in processors, each with its inputs, outputs and options.
- [Glossary](glossary.md): what the terms used here mean.

Coming soon: the pages of the other processors, how Falcon works, the system requirements, installation, a first experiment, example experiments and writing a processor of your own.

## Origins

Falcon began with the work of Davide Ciliberti and Fabian Kloosterman, who introduced it in:

- Ciliberti D, Kloosterman F (2017). Falcon: a highly flexible open-source software for closed-loop neuroscience. *Journal of Neural Engineering* 14, 045004. <https://doi.org/10.1088/1741-2552/aa7526>

Today's Falcon is a new version that builds on that work.
