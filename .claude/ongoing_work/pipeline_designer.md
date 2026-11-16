# Pipeline designer

The part of the UI in which a user builds a graph: places processors on cores, connects them and sets their options. This file is its spec followed by its open questions: a question that is settled moves up into the spec. A section named here without a file, and not found in this one, is in [engine.md](engine.md). What becomes of the graph is in [generator.md](generator.md). It comes after the generator (see **Order of work** at the top of beyond_prototype.md).

## Decided
- **A simple node editor**: The node editor can be simple and fairly static rather than smooth and Figma-like. Adding a processor by clicking it instead of dragging is fine.
- **Placement** is chosen by the user in a single node editor:
  - CPU cores are drawn as areas in the node editor (one core is reserved for the OS and interrupts, chosen from the detected CPU topology; ideally a core outside the large-L3 CCD, e.g. core 15 on a 9950X3D, so all V-cache cores go to Falcon. Falls back to core 0 if the boot CPU's kernel duties can't be moved; to be checked on preempt_rt).
  - The user places each processor in a core's area and draws connections freely between any processors, also across cores. Each processor ends up with a core number.
- **Written in Dear ImGui**: Drop the UI written in Dart, rewrite it in Dear Imgui. The processors and their options for the node editor come from the list a script makes from their source (see **The processors are read from their source by a script** in generator.md), not from C++26 reflection, which clang won't have soon.
- **Cache view**: A cache miss costs ~100 ns versus a few ns for a hit, so a graph that outgrows the CPU cache gets much slower without any visible sign. Show per-core L1/L2/L3 usage (ring buffers + processor state on that core, against cache sizes read from the CPU) in the graph editor while the user designs the graph, before running it. Sizes are calculated, not measured, so users see when their graph stops fitting.
- **Bypass toggle**: for processors with the same input and output type (signal passes through unchanged). Only while the graph is not running.
- **Order of processors**: Let the user set the order of processors on each core in the node editor (see **Per-core loop** in engine.md).

## Open questions
- The first to settle is shared with the generator: what a graph is while it is held in memory, since the node editor fills it and the generator reads it. A rough list of its components is proposed under **The graph's components** in generator.md.
- What the old Dart GUI's editor did, as a list to choose from: dragging a node, clicking a port and then a compatible port to connect them, removing a connection under the cursor, renaming and duplicating a processor, folding a node, zooming, and a panel of processors sorted into inputs, outputs and the rest.
