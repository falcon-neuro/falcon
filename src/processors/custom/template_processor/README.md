# template_processor

A template for a Falcon extension: a folder of processors kept outside Falcon's own, e.g. a lab's private ones or an input built on a vendor's SDK (see **Extensions** in `src/core/spec.md`).

## Starting an extension
1. Copy this folder to `src/processors/custom/<extension name>/`. Falcon's repository ignores everything in `custom/` except this template, so the copy can be a git repository of its own, private if need be.
2. Use the folder name as the C++ namespace of every processor in it (here `template_processor`). It must be a valid C++ name: letters, digits and underscores, not a keyword.
3. Replace `gain.cpp` with your processors, written like Falcon's own in `src/processors/`.
4. If they need a library, add it in `CMakeLists.txt`; Falcon's `CMakeLists.txt` adds every extension's.
5. Put the pipelines that use them in `pipelines/`, since a pipeline that includes a private processor can't be public either.

## Contents
- `gain.cpp`: `Gain`, which multiplies every channel by a constant factor.
- `CMakeLists.txt`: where an extension adds its libraries; this one needs none.
- `pipelines/gain_pipeline.cpp`: SignalGenerator → Gain → SpikeDetector → EventLogger. To run it, point `add_executable(falcon …)` in Falcon's `CMakeLists.txt` at it. It prints 12 spikes per second.
