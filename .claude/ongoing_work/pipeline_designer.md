# Pipeline designer

The part of the UI in which a user builds a graph: places processors on cores, connects them and sets their options. This file is its spec followed by its open questions: a question that is settled moves up into the spec. A section named here without a file, and not found in this one, is in [engine.md](engine.md). What becomes of the graph is in [generator.md](generator.md). It comes after the generator (see **Order of work** at the top of beyond_prototype.md).

## Decided
- **No free placement** (decided by the user). The editor is not an empty canvas on which nodes are moved about, as the old Dart GUI's was, which made placing nodes more complex than it needed to be. Each processor and each connection is drawn at a place that follows from the graph alone, so no coordinates are kept anywhere.
  - **CPU cores are areas.** Which cores are Falcon's is what the machine's setup isolated (see **A core that isn't isolated is marked**); choosing them, ideally leaving the OS a core outside the large-L3 CCD, e.g. core 15 on a 9950X3D, is that setup's task (see task A.1 under **Tasks** in beyond_prototype.md). The user drags a processor from the panel and drops it into a core's area, which gives it its core.
  - **Dragging only brings a processor into existence**: the user drags it from the panel and drops it into a core's box, where it sits unconnected. A drop never makes a connection.
  - **Every connection is made with two clicks**: on an output of one processor, then on an input of another, on the same core or on another. Between two cores the generator makes of it what crosses cores, a ring or a cross-core state value (see **Rings** and **State values** in engine.md).
  - **The layout follows the connections**, top to bottom: once two processors of one core are connected, the one that takes the input is drawn under the one that gives the output. A chain is what comes of that; the user never builds or marks one (see **Chains are detected automatically** in generator.md).
    - **An output read by several processors** of its core fans out: they are drawn side by side under it.
    - **A processor with inputs from two processors** sits in the middle under both, which are drawn with room between them.
    - **A processor not yet connected** sits at the top of its core's box, beside the top of the chains already there.
    - **A state value's connection places nothing** (decided by the user): only connections of samples and events place the blocks, and a state value's is a line drawn beside them. State values can run in a circle on one core, which a layout from top to bottom couldn't hold; one that comes from further down is read one pass late (see **Order** under **Per-core loop** in engine.md).
    - **A connection between cores** leaves the core of the processor that gives it at the bottom of that core's box and enters the reading core's box from the top, so its line has the shape of an S. The processor that reads it has nothing above it in its own core and sits at the top.
  - **Only the cores in use have a box** (decided by the user): one box when the pipeline is empty, then one for each core that holds a processor. While a processor is dragged from the panel, the next free core's box appears as one more place to drop it, and stays only if the processor is dropped there.
  - **A core that isn't isolated is marked** (decided by the user). The editor reads from the kernel which cores are isolated from the OS. A box of one that isn't has a warning colour and says "not isolated", with a tooltip: the OS and its interrupts run there, so a processor on it can be held up at any moment; one that isn't realtime, such as a recorder, is fine there. A line above the boxes names the isolated cores, and its tooltip the others. It warns and doesn't forbid. Isolated cores are offered first, lowest number first, then the others.
  - **The cores' area scrolls sideways during a drag** (decided by the user) when the mouse nears its left or right edge, so that a core beyond the window's width can be reached.
  - **A processor that exists can't be dragged about** (decided by the user). A drag does one of two things: left or right, it moves the whole chain the processor belongs to among the chains of its core, which sets the order they run in; into another core's box, it moves that one processor there. Its connections stay as they are, and those that now cross cores become rings or cross-core state values. A drag released anywhere else puts everything back where it was.
  - **A core's chains run from left to right**: that is their order in the core's loop, and the one thing about the layout that the connections don't give, so it is kept with the graph.
  - Every place is one of a fixed set inside a core's box, so nothing has coordinates.
  - The editor can be simple and fairly static rather than smooth and Figma-like. Adding a processor by clicking it instead of dragging is fine.
- **Written in Dear ImGui**: Drop the UI written in Dart, rewrite it in Dear Imgui. The processors and their options for the node editor come from the list a script makes from their source (see **The processors are read from their source by a script** in generator.md), not from C++26 reflection, which clang won't have soon.
- **Cache view**: A cache miss costs ~100 ns versus a few ns for a hit, so a graph that outgrows the CPU cache gets much slower without any visible sign. Show per-core L1/L2/L3 usage (ring buffers + processor state on that core, against cache sizes read from the CPU) in the graph editor while the user designs the graph, before running it. Sizes are calculated, not measured, so users see when their graph stops fitting.
- **Bypass toggle**: for processors with the same input and output type (signal passes through unchanged). Only while the graph is not running.
- **Order of processors**: inside a chain it follows from the connections, and between a core's chains the user sets it by dragging a chain left or right (see **A core's chains run from left to right**).

## Done
- **The main page is the pipeline designer** (`src/ui/pipeline_designer/pipeline_designer.cpp`), with the ephys viewer hidden for now, its code untouched; tabs between the two come later (decided by the user).
- **The panel of processors** at the left (`processors_panel.cpp`) shows every processor of `all_processors.cpp` (see **Done** in generator.md): opened, its description, its file, its options with type and default, and its connections with direction and kind. The "(i)" beside an option or connection shows its description.
- **A box for each core in use**, side by side, with the next free core's box shown during a drag (see **Only the cores in use have a box**). A processor dragged from the panel into a box comes into existence there: it is added to the graph (see **The graph as code** under **Done** in generator.md) and drawn at the top of the box, beside those already there, with its name and its type. The box is marked while a processor is dragged over it.
- **Ports on a block**: its inputs on top, its name and type in the middle, its outputs at the bottom, as the data flows. Each port is a dot in the colour of what it carries (samples blue, events orange, a state value green, a port with slots grey) and its name, fainter when it may stay unconnected; its tooltip says what it is and shows its description.
- **Which cores are isolated** is read from `/sys/devices/system/cpu/isolated` (`cpu_cores.cpp`), and the number of cores from `present` there, not from the cores the program may run on, which leaves the isolated ones out.
- **Connecting**: a click on an output picks it, and a line follows the mouse from it; the inputs it fits are marked and the others can't be clicked; a click on a marked input makes the connection. A second click on the output, or Escape, lets it go. A right click on an input removes its connections, a first way to do that and not yet decided. A line leaves its output's block at the bottom and reaches its input's block from above, in the colour of what it carries, so between two cores it has the shape of an S.
- What "fits" checks so far: that the two ports carry the same kind, samples, events or a state value, that the input is free or has slots, and that the two are ports of different processors. Not yet: a stream's channels and rate or an event's type (see **Types across the graph** in generator.md).
- Built; not yet looked at by Claude, who can't see the app. Not there yet: the layout that follows the connections, so a connected processor still sits in the top row; moving and removing a processor; its options.
- Its lint needs one build first, since it includes the file the build generates.
- **The window's title bar** was missing under GNOME on Wayland, where an app draws its own through libdecor: the library was installed but not the plugin that draws the bar (the app says "No plugins found, falling back on no decorations"). `scripts/setup/install_deps.sh` now installs `libdecor-0-plugin-1-gtk`.

## Next
Where the work stopped on 16 November 2026, for whoever continues. In this order:
1. **The layout that follows the connections**, decided under **The layout follows the connections** and not built: every processor of a core still sits in its top row, whatever it is connected to. To build, for each core: a processor goes one row under what it reads, counting connections of samples and events only; an output's readers side by side under it; a reader of two processors in the middle under both; the chains from left to right in the order of the core's list in the graph. Blocks differ in height, by their ports, so a row is as high as its tallest block.
2. **Moving and removing a processor**: a drag left or right moves its chain among its core's chains, a drag into another core's box moves the processor there, and any other drag puts it back (see **A processor that exists can't be dragged about**). Removing one is not discussed yet.
3. **A processor's options** in the editor, with importing from a file, and the compiler's check of them, whose precompiled header is not built yet (see **A file is only imported from** and **The compiler checks a processor's options** in generator.md).
4. **Types across the graph**, then the generator writing `pipeline.cpp` (see the open questions of generator.md).

Waiting for the user:
- Whether connecting works as built: asked, not answered yet. Everything before it the user ran and found working.
- The texts written for every processor, option and connection, about 60, to review (see **The texts** under **Done** in generator.md).
- The window's title bar comes back once the user installs the plugin (see **The window's title bar** under **Done**), which needs root.

The app is `src/main.cpp`, which `CMakeLists.txt` names by hand as the program to build; an engine pipeline is built by naming it there instead.

## Later
- **Moving a whole chain to another core** at once, where a drag moves one processor: by Ctrl and a drag, or by a bar on top of the chain that moves all of it, much like the group around a rack of audio effects in Ableton Live.

## Open questions
- **What follows from a fixed layout**, to design:
  - The order from left to right where neither the connections nor the user give it: of the processors an output fans out to.
  - The cores' boxes stand side by side by core number, in a row that scrolls (in code). Not looked at yet: many cores at once, and where a line between two cores far apart runs.
  - How a connection is removed is to decide: the code's right click on an input is a first way. So is how a processor is removed, not discussed yet. And, for a ring, where its number of slots is set.
  - A circle of samples or events on one core, e.g. two processors feeding each other, which the layout from top to bottom can't hold: whether the editor refuses the connection that closes it.
  - Of what the old Dart GUI's editor did, still of use: renaming and duplicating a processor, and the panel of processors. No longer needed: dragging a node anywhere, zooming, folding a node.
