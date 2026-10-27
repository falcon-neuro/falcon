# Falcon
Develop new version of Falcon (_Ciliberti & Kloosterman, 2017 J. Neural Eng. 14 045004_) that uses modern C++ to handle high channel count high data rate (+30khz) on CPU for realtime closed loop neural processing and experiments.

## Overview

- Goal is low latency. Every nanosecond matters. ASIC or FPGA can be faster but not flexible for closed-loop BCI experiment design. Falcon squeezes every bit of modern x86 CPU to be fast yet stays flexible with drag-and-drop experiment design UI.
- Main use case is heavy computational work such as online hippocampal decoding for sub-1ms closed-loop experiments, such as memory impairment or improvement.
- Secondary use case is basic closed-loop experiments such as spike detection or burst detection. Falcon is desirable for such basic experiments because it provides flexible experimental design while doing the heavy lifting for the low level communication to acquisition hardware such as Neuralynx, Blackrock, Ripple Neuromed, TDT, Neuropixels and more to come.
- Falcon runs on Debian with preempt_rt, with 1 core handling OS and interrupts while all the remaining cores are dedicated to Falcon only.
- Falcon requires at least AVX2 X86 CPU with `constant_tsc`. For heavy pipelines, we recommend 9950X3D for AVX512, CL30 memory and such. Labs do buy our recommended high performance hardware to have the lowest latency possible. Overall, we are safe to assume that the CPU will be post-2020.
- We want flexibility, user friendliness, scalability for massive electrode arrays.
- Only .cpp files, for minimalism.
- Falcon gets it's flexibility from it's engine that run a graph composed of "processors". Example processors are NeuropixelsInput, NeuralynxInput, Filter, SpikeDetector, PCIeParallelOutput. Users design the graph they need for their experimental needs.

## In progress
- Prototype spec: [src/core/spec.md](../src/core/spec.md).
- After the prototype: [src/core/beyond_prototype.md](../src/core/beyond_prototype.md).
- Graph shapes atlas: [src/core/graph_shapes.md](../src/core/graph_shapes.md).

## Guidelines
- Before deep thinking, get the higher level picture first and collaborate with me so I can give you directions to dive into.
- If there is a possibility that you will misunderstand me or assume something wrong, do a preemptive check by asking me. If needed, we may collaborate for diagnostics code where you generate it and I run it and tell you the result. Never assume things.
- Don't generate a huge output unless I ask you to do so. Always start high level and ask permission to go for details. This will help us align from start and prevent wasting time.
- Only generate code, do not perform any other action such as building or running code.
- Planning, including plan mode, is brainstorming. Never prompt me with selection prompts (multiple-choice questions); we discuss in plain text.
- Always start slow and high level, without diving deep. Only after we have discussed the high level do we go into specifics.
- We are dealing with complex software architecture: there are no easy solutions, only trade-offs. If the answers were easy, I wouldn't brainstorm with you, so don't present a choice as obvious; work through the trade-offs with me.