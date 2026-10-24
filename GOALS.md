# Falcon
Develop new version of Falcon (_Ciliberti & Kloosterman, 2017 J. Neural Eng. 14 045004_) that uses modern C++ to handle high channel count high data rate (+30khz) on CPU for realtime closed loop neural processing and experiments.

## Overview

- Goal is low latency. Every nanosecond matters. ASIC or FPGA can be faster but not flexible for closed-loop BCI experiment design. Falcon squeezes every bit of modern x86 CPU to be fast yet stays flexible with drag-and-drop experiment design UI.
- Main use case is heavy computational work such as online hippocampal decoding for sub-1ms closed-loop experiments, such as memory impairment or improvement.
- Secondary use case is basic closed-loop experiments such as spike detection or burst detection. Falcon is desirable for such basic experiments because it provides flexible experimental design while doing the heavy lifting for the low level communication to acquisition hardware such as Neuralynx, Blackrock, Ripple Neuromed, TDT, Neuropixels and more to come.
- Falcon runs on Debian with preempt_rt, with 1 core handling OS and interrupts while all the remaining cores are dedicated to Falcon only.
- Falcon requires at least AVX2 X86 CPU. For heavy pipelines, we recommend 9950X3D for AVX512, CL30 memory and such. Labs do buy our recommended high performance hardware to have the lowest latency possible.
- We want flexibility, user friendliness, scalability for massive electrode arrays.
- Only .cpp files, for minimalism.
- Falcon gets it's flexibility from it's engine that run a graph composed of "processors". Example processors are NeuropixelsInput, NeuralynxInput, Filter, SpikeDetector, PCIeParallelOutput. Users design the graph they need for their experimental needs.

## In progress
- **A:**  Refactor Falcon engine
    1. Drop old lmax distruptor.
    2. Architect SPSC architecture that minimizes cache misses.
    3. Architect process scheduling engine for maximizing performance on all cores that dedicated to Falcon. Detect CPU microarch on the fly and change grub settings to isolate all cores but one for Falcon.
- **B:** Use SIMD
    1. Refactor existing scalar processors to SIMD using AVX512, with fallback to AVX2. Scalar implementation should be dropped and AVX2 should be mandatory to run Falcon.
- **C:** JIT
    1. Replace existing YAML based graph definition with "poor-mans-JIT".
    2. Ship with clang compiler and compile the graph from the generated .cpp file.
- **D:** UI
    1. Make scientifically accurate multichannel viewer that never drops a frame and up to modern neuroscience ephys standards.
    2. Minimalize CPU usage to absolute minimum by using OpenGL 4.6.0 and use shaders for rendering waveforms on the screen.
    3. Implement audio output from ephys signal, offer custom signal channel selection to user and merge the selected channels and send them to speaker.
    4. Drop the GUI written in Dart, rewrite it in Dear Imgui. Use C++26 reflection to get processors and