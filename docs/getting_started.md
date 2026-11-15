# Overview {#mainpage}

Falcon is open-source software for closed-loop neuroscience experiments. It reads neural signals from an acquisition system while they are being recorded, runs them through a pipeline of processors, such as filters and detectors, and triggers an output, for example a TTL pulse to a stimulator, when the pattern of interest appears. Each experiment assembles its own pipeline from these processors.

Falcon runs on Linux and is released under the GNU General Public License v3. The source code is on [GitHub](https://github.com/falcon-neuro/falcon).

## Status

A new engine is in development. It is designed for dense recordings, with hundreds of channels sampled at 30 kHz, processed sample by sample on a standard CPU.

No installable release is available yet. Installation instructions and the user guide will be published here with the first release of the new engine.

## Publications

Falcon was introduced in:

- Ciliberti D, Kloosterman F (2017). Falcon: a highly flexible open-source software for closed-loop neuroscience. *Journal of Neural Engineering* 14, 045004. https://doi.org/10.1088/1741-2552/aa7526

It was used to detect the content of hippocampal replay in real time in:

- Ciliberti D, Michon F, Kloosterman F (2018). Real-time classification of experience-related ensemble spiking patterns for closed-loop applications. *eLife* 7, e36275. https://doi.org/10.7554/eLife.36275
