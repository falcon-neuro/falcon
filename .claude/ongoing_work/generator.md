# Generator

The part of Falcon that turns a graph into `pipeline.cpp`, the file an engine program is compiled from. This file is the generator's spec followed by its open questions: a question that is settled moves up into the spec. A section named here without a file, and not found in this one, is in [engine.md](engine.md). The node editor that hands it the graph is in [pipeline_designer.md](pipeline_designer.md).

## Decided
- **Where it is built**: in `src/core`, designed by Claude, with no UI yet: C++ that turns a graph held in memory into `pipeline.cpp`.
- **What it writes** is the engine's `pipeline.cpp` (see **pipeline.cpp** in engine.md, for its layout, its loops and its names). Until it exists, `pipeline.cpp` is written by hand (see **Prototype plan** in engine.md).
- **UI and engine are separate programs** (see **Boundaries** in engine.md). The UI is the frontend. It generates `pipeline.cpp`, compiles it with the bundled clang into an engine executable, and runs that as a child process.
- **Graph definition** is a generated .cpp file (`pipeline.cpp`), compiled with the bundled clang (JIT plan).
  - The generator is simple drag-and-drop editor logic: after each edit to the graph in the UI, it regenerates `pipeline.cpp`. Users only place and connect processor nodes; they never write `pipeline.cpp`.
  - The UI re-opens a saved graph by reading `pipeline.cpp` back, so the file follows a fixed generated format. Node positions are stored as comments at the end of the file; if they are missing, the UI places the nodes itself.
- **Chains are detected automatically** by the generator each time it regenerates `pipeline.cpp` (see **Chains** in engine.md). The user never marks chains.
- Only processors placed on a core are written to the generated `pipeline.cpp`; everything else does not run. Empty cores stay idle, which lowers heat.
- **A file given as an option stays a file** (the user's rule), e.g. a filter's coefficients file. The file is copied into the project's folder, and the graph holds only a link to that copy. The generator reads the file and takes the values out of it each time it writes `pipeline.cpp`, never earlier. So the user can always take the file back out of the project's folder into Python or MATLAB and compare. The project's copy is the one that counts: a run's directory gets no copy of its own (decided by the user). Not chosen: reading the values when the file is picked and keeping them as a text option, since the file could then not be had back.
- **The processors are read from their source by a script** (decided by the user). Clang won't have C++26 reflection any time soon, so a script reads each processor's definition, its options, its connections and its template parameters, from `src/processors/`. Proposed by the user as its language: Python.
- **The script is a step of the build** (decided by the user). CMake runs it first: it reads the processors' folder and writes one C++ file, `all_processors.cpp` or a name like it, which the UI and the generator are then compiled with. So a processor added to the folder shows in the UI's panel after the next build, with nothing else to edit.

## Tasks
From the former TODO.md:
1. Replace existing YAML based graph definition with "poor-mans-JIT".
2. Ship with clang compiler and compile the graph from the generated .cpp file.
3. Generate one loop per core in `pipeline.cpp` (see **Per-core loop** in engine.md).

## Open questions
- **Files given as options**, what the rule under **Decided** leaves open:
  - A link that no longer holds: the file is gone, or was changed since the last time it was read. The first must stop the generator with a clear message; whether the second is told to the user is open.
  - Reading `pipeline.cpp` back: it holds the values the generator wrote, so it must also hold the link, e.g. in a comment beside them, for the UI to show the file again and not the values.
  - Exact values: the generator must write each value so that the compiled number is bit for bit the one in the file, or a comparison with Python or MATLAB differs in the last digit.
  - A reader per kind of file in the generator (IIR sections, FIR taps, later an IMRO table), and whether the file picker checks a file as well when it is picked, e.g. its `type` and `sampling_rate` (see **IirFilter** in engine.md).
- **The graph's components**, a rough first list, proposed and to confirm. It is what the node editor fills and the generator reads. The old Dart GUI (`falcon_gui/lib/model/falcon_graph.dart`) had the first five in some form; what it lacked is marked new.
  - **Processor type**: what a kind of processor is, apart from any graph: its C++ name and file, its template parameters, its options and its ports. The old GUI kept these as templates written by hand in Dart, a second copy of the C++ that could drift from it.
  - **Option**: a name, a type (whole number, number, yes or no, one of a list, text, file) and a value, written into `pipeline.cpp` as a compile-time constant. As in the old GUI, without its YAML kinds. A file option's value is a link to a file in the project's folder, not what the file holds (see **A file given as an option stays a file**).
  - **Port**: one member of a processor's `Connections`: its name, its direction, and its kind. New: the kind is samples, events or a state value, where the old GUI had one kind of port plus a flag for states, never validated, so it had no live change of a state either. New too: whether the port must be connected, and whether it has slots, so takes several connections.
  - **Processor**: one node of a graph: its name, which is its variable's name in `pipeline.cpp`, its type, its options' values and its position in the editor. New: the core it sits on and its place in that core's order.
  - **Connection**: from one processor's output port to another's input port, of the kind its ports have. New, and worked out, not set by the user: whether it stays on a core (a buffer or a state value) or crosses cores (a ring or a cross-core state value), and its name. New, set by the user: a ring's number of slots.
  - **Core**, new: its number, whether it is realtime, and its processors in order. The old GUI had none, since Falcon 1 ran a thread per processor.
  - **Stream**, new: what a sample connection carries, a channel count, a rate and a sample type. It starts at an input processor and follows the connections, so that only matching ports connect and the types can be written. The old GUI compared two type names as text.
  - **Pipeline**: the whole graph and its name, which names its runs' directories.
  - Later, with the bridge to the UI (see **Order of work** in beyond_prototype.md): a state input that the UI writes during a run.
  - Worked out by the generator from these, never edited: buffers, rings, cross-core state values, each core's loop, the includes and the type names.
- **Its three parts**, as seen so far, none designed yet: knowing every processor's connections, options and template parameters; working out each connection's type across the graph (channel count, rate, event type) as the compiler would, so that only matching ends connect and the types can be written; and writing `pipeline.cpp` in its fixed form and reading it back, node positions included.
- **Its test**, proposed, to confirm: it writes an existing hand-written pipeline again, e.g. `ripple_detector_pipeline.cpp`, text for text after formatting.
- **Knowing the processors**, what the script under **Decided** leaves open:
  - How it reads the C++. By patterns in the text: simple, and the processors follow one strict form, but a declaration it doesn't expect is skipped or misread without an error. Or by asking clang for its syntax tree of the file (clang can print it as JSON): the script then parses no C++ itself and sees what the compiler sees, but depends on that output, and a member of a template's `Connections` comes as written, not as a type worked out. Claude leans to the syntax tree; whether it holds what is needed, e.g. each option's default, is to check on one processor first.
  - Python, to confirm. For it: quick to write, it is off the realtime path, and Falcon needs Python on the machine anyway, for filter design (see **Filter design** under **UI** in beyond_prototype.md). Against it: the generator is C++ (see **Where it is built**), so the list of processors crosses from one language to the other.
  - What the file it writes holds: a description of each processor as data (its name, its file, its template parameters, its options with their types and defaults, its ports with their kinds), not the processors themselves, which only `pipeline.cpp` includes. Its name, since `all_processors.cpp` could be read as the processors themselves.
  - Where that file goes: into the build's folder, so that it is never edited and never checked in out of date.
  - CMake noticing a file added to the folder, not only a changed one, so that the script runs again.
  - A UI to build: the build has one target today, the pipeline at hand (see **Tests as whole runs** in test_suite.md, which meets the same limit).
  - A lab's own processor (see **Extensions** in engine.md): it shows in the UI only after the UI is built again, so that lab needs Falcon's source and build tools, not only the installed program.
  - Where it lives: `src/core` holds only C++ and `scripts/` only shell scripts.
