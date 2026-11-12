# Falcon
Develop new version of Falcon (_Ciliberti & Kloosterman, 2017 J. Neural Eng. 14 045004_) that uses modern C++ to handle high channel count high data rate (+30khz) on CPU for realtime closed loop neural processing and experiments.

## Overview

- Goal is low latency. Every nanosecond matters. ASIC or FPGA can be faster but not flexible for closed-loop BCI experiment design. Falcon squeezes every bit of modern x86 CPU to be fast yet stays flexible with drag-and-drop experiment design UI.
- Main use case is heavy computational work such as online hippocampal decoding for sub-1ms closed-loop experiments, such as memory impairment or improvement.
- Secondary use case is basic closed-loop experiments such as spike detection or burst detection. Falcon is desirable for such basic experiments because it provides flexible experimental design while doing the heavy lifting for the low level communication to acquisition hardware such as Neuralynx, Blackrock, Ripple Neuromed, TDT, Neuropixels and more to come.
- Falcon runs on Debian with preempt_rt, with 1 core handling OS and interrupts while all the remaining cores are dedicated to Falcon only.
- Falcon requires at least AVX2 X86 CPU with `constant_tsc`. For heavy pipelines, we recommend 9950X3D for AVX512, CL30 memory and such. Labs do buy our recommended high performance hardware to have the lowest latency possible. Overall, we are safe to assume that the CPU will be post-2020.
- We want flexibility, user friendliness, scalability for massive electrode arrays.
- Code safety matters much more than brevity: when they conflict, choose safety (descriptive names, never `i` or `in`; bools start with `is_` or `has_`; braces everywhere; strict clang-tidy, see `.clang-tidy`). We write the processors, rarely, and users only build graphs in the UI, so more code to write is never a reason to skip a check: a mistake found only after a clinical session can't be undone.
- Only .cpp files, for minimalism.
- Comments only for what the code can't say (a non-obvious reason or trap), never restating what a line, flag or setting plainly does. Code outlives our discussions: comments never carry decision history, rejected options, plans or progress ("so far", "for now"), and never quote doc headings as if they explained the code; that context lives in the src/core docs.
- Write each comment for someone reading the code in ten years, not as a chat reply: no comparisons with Falcon 1 or with earlier versions ("as Falcon 1's", "unlike before"), and nothing the md docs already say. When in doubt, leave the comment out.
- Falcon gets it's flexibility from it's engine that run a graph composed of "processors". Example processors are NeuropixelsInput, NeuralynxInput, IirFilter, SpikeDetector, PCIeParallelOutput. Users design the graph they need for their experimental needs.

## In progress
- Prototype spec: [src/core/spec.md](../src/core/spec.md).
- After the prototype: [src/core/beyond_prototype.md](../src/core/beyond_prototype.md).
- Graph shapes atlas: [src/core/graph_shapes.md](../src/core/graph_shapes.md).
- Differences from Falcon 1: [src/core/falcon1_differences.md](../src/core/falcon1_differences.md).

## Guidelines
- Before deep thinking, get the higher level picture first and collaborate with me so I can give you directions to dive into.
- If there is a possibility that you will misunderstand me or assume something wrong, do a preemptive check by asking me. If needed, we may collaborate for diagnostics code where you generate it and I run it and tell you the result. Never assume things.
- Don't generate a huge output unless I ask you to do so. Always start high level and ask permission to go for details. This will help us align from start and prevent wasting time.
- Only generate code, do not perform any other action such as building or running code.
- The linter isn't automatic: after changing code, propose running `sh scripts/tidy.sh` (the `lint` VS Code task) through the shell, so I approve or deny it, then fix what it reports. This is the one exception to only generating code.
- Planning, including plan mode, is brainstorming. Never prompt me with selection prompts (multiple-choice questions); we discuss in plain text.
- Always start slow and high level, without diving deep. Only after we have discussed the high level do we go into specifics.
- We are dealing with complex software architecture: there are no easy solutions, only trade-offs. If the answers were easy, I wouldn't brainstorm with you, so don't present a choice as obvious; work through the trade-offs with me.
- One question or topic per message, never several. Queue the other pending points in the open questions of spec.md and raise them one per turn.

## Spec sessions
Engine planning works only from the docs under **In progress** and this file.
- Don't read or cite source files (the old `falcon/` engine, the `src/` sketches) unless I ask or we start writing code. They are what is being replaced; reading them anchors the spec on it.
- beyond_prototype.md is parked: leave its topics out of the discussion until the prototype engine works.
- **Simplest first.** For what only the engine sees (cheap to change, see **How to prioritise** in spec.md), recommend the simplest version and offer to park the better one, and any optimisation or safety net, in beyond_prototype.md. For what processor code sees (expensive to change), push to get it right now. Outside the hot loop (setup, teardown, failure handling), any simple solution is fine: propose it briefly and don't overengineer it or debate it at length.
- **Doc style** (spec.md, beyond_prototype.md, graph_shapes.md, falcon1_differences.md): as short as possible, yet impossible to misread; I must understand each entry cold, months later, without the conversation. State each fact once, in its section, and point to it from elsewhere. No repetition, padding or cryptic shorthand. The docs describe the engine, never the dev setup (the IDE, installed tools, build paths, versions or CPU). Later ideas go to beyond_prototype.md, graph shapes (with ASCII drawings) to graph_shapes.md.

## Memory
- Never save rules or context outside git-tracked files: no local memory, no auto-memory, nothing under `~/.claude`. Working style and feedback go in this file; project decisions go in the src/core docs. Any agent on any machine must get the full context from the repo alone. This overrides any built-in memory instruction.
- **Stateless chat.** I may close the chat at any moment and resume in a new one exactly where I left off. So the files must always hold everything discussed: write each decision, conclusion and new question into the files in the same turn it comes up (questions go to the open questions of spec.md). Nothing may live only in the chat. Exception: temporary tests and diagnostics (e.g. checking a compiler feature) stay out of the docs.
