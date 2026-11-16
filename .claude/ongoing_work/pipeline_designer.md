# Pipeline designer

The part of the UI in which a user builds a graph: places processors on cores, connects them and sets their options. This file is its spec followed by its open questions: a question that is settled moves up into the spec. A section named here without a file, and not found in this one, is in [engine.md](engine.md). What becomes of the graph is in [generator.md](generator.md). It comes after the generator (see **Order of work** at the top of beyond_prototype.md).

## Decided
- **No free placement** (decided by the user). The editor is not an empty canvas on which nodes are moved about, as the old Dart GUI's was, which made placing nodes more complex than it needed to be. Each processor and each connection is drawn at a place that follows from the graph alone, so no coordinates are kept anywhere.
  - **CPU cores are areas** (one core is reserved for the OS and interrupts, chosen from the detected CPU topology; ideally a core outside the large-L3 CCD, e.g. core 15 on a 9950X3D, so all V-cache cores go to Falcon. Falls back to core 0 if the boot CPU's kernel duties can't be moved; to be checked on preempt_rt). The user drags a processor from the panel and drops it into a core's area, which gives it its core.
  - **Chained processors sit next to each other**, like the blocks of Scratch: where a processor is put among the others of its core is its place in that core's order (see **Order** under **Per-core loop** in engine.md).
  - The editor can be simple and fairly static rather than smooth and Figma-like. Adding a processor by clicking it instead of dragging is fine.
- **Written in Dear ImGui**: Drop the UI written in Dart, rewrite it in Dear Imgui. The processors and their options for the node editor come from the list a script makes from their source (see **The processors are read from their source by a script** in generator.md), not from C++26 reflection, which clang won't have soon.
- **Cache view**: A cache miss costs ~100 ns versus a few ns for a hit, so a graph that outgrows the CPU cache gets much slower without any visible sign. Show per-core L1/L2/L3 usage (ring buffers + processor state on that core, against cache sizes read from the CPU) in the graph editor while the user designs the graph, before running it. Sizes are calculated, not measured, so users see when their graph stops fitting.
- **Bypass toggle**: for processors with the same input and output type (signal passes through unchanged). Only while the graph is not running.
- **Order of processors**: the user sets the order of the processors on each core by where the blocks are put (see **No free placement**).

## Done
- **A panel that lists the processors**, `src/ui/main_window/processors_panel.cpp`, at the left of the main window beside the ephys viewer. It shows every processor of `all_processors.cpp` (see **Done** in generator.md): opened, its description, its file, its options with type and default, and its connections with direction and kind. The "(i)" beside an option or connection shows its description. Built; not yet looked at by the user. Nothing can be placed or connected yet.
- Its lint needs one build first, since it includes the file the build generates.

## Open questions
- The first to settle is shared with the generator: what a graph is while it is held in memory, since the node editor fills it and the generator reads it. A rough list of its components is proposed under **The graph's components** in generator.md.
- **What follows from a fixed layout**, to design:
  - Whether putting one block right after another also connects them, the first's output to the second's input, or only sets their order, with every connection made by hand.
  - How a connection that isn't to the next block is drawn at a place that follows from the graph: an output read by several processors, a processor with several inputs (samples and a state value), and a connection that crosses cores.
  - Several chains on one core, which run in one loop in one order.
  - What the old Dart GUI's editor did is mostly not needed then: dragging a node anywhere, zooming, folding a node. Still of use: clicking a port and then a compatible port to connect them, removing a connection, renaming and duplicating a processor, and the panel of processors.
