# Beyond the prototype

Everything that is not part of the prototype engine: features, ideas and tasks for after the prototype works. The prototype spec is in [spec.md](spec.md). We leave all of this out of the engine spec discussion until the prototype engine works. Each item keeps the status it had before it moved here (decided, open or deferred).

Moved here on Claude's judgment and not yet confirmed: **Heavy processors**, **Several chains on one core**, **Feedback connections**, **Buffer reuse**, **Event rings between cores**, **Slow readers**, **Experiment files**, **Channel labels and positions** and **Rings with many readers**. Whatever belongs in the prototype moves back to spec.md.

## Engine

### Generator
- The generator, and with it the UI reading `graph.cpp` back (see **Graph definition** below). The prototype uses a hand-written `graph.cpp` (see **Prototype plan** in spec.md).
- **UI and engine are separate programs** (see **Boundaries** in spec.md). The UI is the frontend. It generates `graph.cpp`, compiles it with the bundled clang into an engine executable, and runs that as a child process.
- **Graph definition** is a generated .cpp file (`graph.cpp`), compiled with the bundled clang (JIT plan).
  - The generator is simple drag-and-drop editor logic: after each edit to the graph in the UI, it regenerates `graph.cpp`. Users only place and connect processor nodes; they never write `graph.cpp`.
  - The UI re-opens a saved graph by reading `graph.cpp` back, so the file follows a fixed generated format. Node positions are stored as comments at the end of the file; if they are missing, the UI places the nodes itself.
  - The node editor can be simple and fairly static rather than smooth and Figma-like. Adding a processor by clicking it instead of dragging is fine.
- **Placement** is chosen by the user in a single node editor:
  - CPU cores are drawn as areas in the node editor (one core is reserved for the OS and interrupts, chosen from the detected CPU topology; ideally a core outside the large-L3 CCD, e.g. core 15 on a 9950X3D, so all V-cache cores go to Falcon. Falls back to core 0 if the boot CPU's kernel duties can't be moved; to be checked on preempt_rt).
  - The user places each processor in a core's area and draws connections freely between any processors, also across cores. Each processor ends up with a core number.
- **Chains are detected automatically** by the generator each time it regenerates `graph.cpp` (see **Chains** in spec.md). The user never marks chains.
- Only processors placed on a core are written to the generated `graph.cpp`; everything else does not run. Empty cores stay idle, which lowers heat.
- The generator's tasks are C under **Tasks**.

### Splittable processors
Decided:
- **Splittable processors** let a heavy processor (e.g. a decoder) use several cores.
  - A processor that supports it shows a split button in the node editor. Splitting adds a part, connected to the original by a visible line showing they are one processor.
  - Parts are placed like any processor, but two parts of one processor can never be on the same core.
  - The original part runs `process()`. It hands out the work, does its own share and combines the results.
  - Each other part runs `delegate(i)` (working name) in its core's loop. `delegate(i)` has the same shape as `process()`: it returns at once if there is no new work.
  - The processor decides how its work is split (electrode groups, position bins, …), since only it knows what splits well.
  - It pays off only for processors that take tens of µs or more. Handing out work and collecting results costs core crossings, so splitting a filter costs more than it saves.
  - Each part keeps its share of the state in its own core's L2, so a decoder on 4 cores gets 4 MB of L2.

```cpp
Decoder decoder {...};                                              // shared by its parts
void core4() { while (running) decoder.process(bins_to_decoder); }  // original part
void core5() { while (running) decoder.delegate(1); }               // split-off part
```

Open (formerly open question 6):
- How many parts are allowed: decide once the data passed through the pipeline is clearer.
- Which parts read the input:
  - a. only the original part, which hands out the work (one extra core crossing)
  - b. every part directly: each part notices new data itself, so only collecting the results costs a crossing
- Can a part's core run other processors too? It works, but the original part waits for the slowest part, so other work on those cores delays the processor.
- A final name for `delegate(i)` (`help` was rejected).

### Extensions
Decided:
- **Only extensions are installed machine-wide** (e.g. a lab's private processors).
- **Extensions live in `src/processors/custom/<extension name>/`**, one folder per extension. The folder name is the extension's C++ namespace, so processors from different extensions can share a name without clashing.
  - Falcon ships only the processors directly in `src/processors/`.
  - `custom/` is git-ignored except `custom/template/`: a template extension with a dummy processor that labs copy to start their own.
  - The template is written once the processor interface is settled (see **Processors** in spec.md).
- **Extension versioning is the lab's responsibility.** Falcon doesn't track extension versions, since an extension folder may not be a git repo. A missing extension still shows up as a compile error naming its namespace when `graph.cpp` is built.

### Processor type alternatives
For now a processor is a plain struct with public fields (see **Processors** in spec.md). Options for later:
- Options in a nested struct (`Filter ap {{.highpass = 300}}`), so the processor's internal state can be private.
- A concept that checks at compile time what every processor must provide, so lab authors get a clear error. Since each processor's inputs differ, it can only check the parts they all share (e.g. that options are primitives or file paths).

### Rules that fail silently
A processor author must copy what they need to keep and never keep a pointer into a buffer (see **Buffers** in spec.md). Breaking this rule gives wrong triggers and no error, and keeping such a pointer compiles without a warning. When it breaks depends on where the buffer points: an engine-owned buffer is overwritten on the next pass, so the bug shows at once; a ring slot is overwritten only when the ring wraps around (e.g. 64 samples later), so a short test passes. The same processor can therefore work on one core layout and give wrong values on another.
- Mostly prevented already: processors read samples only through the buffer's load/store helpers (see **Sample type** in spec.md), so they never get a raw pointer to keep.
- Still possible: keeping a reference to a buffer and reading it in a later pass, when it already holds newer data or nothing. Candidate safety net: a debug build that fills every buffer with NaN after its pass, so such a mistake shows at once in a test. Release builds pay nothing. Not needed for the prototype, whose processors we write ourselves.

### Sample width: float16
The prototype uses float32 (see **Sample type** in spec.md). Processors load and store samples through the buffer's helpers, so switching to float16 later changes only the stream type, not any processor. Still open: whether to switch.
- The format must hold: spikes within about ±500 µV, LFP up to a few mV, and stimulation artifacts that drive the ADC to its full range (e.g. ±8 mV on Blackrock).
- **float32**: the helpers are plain loads and stores, with no conversions. 32 bits is more than the signal needs: acquisition ADCs deliver 10 to 24 bits, and the noise floor limits the useful resolution further.
- **float16** (IEEE half precision): half the bytes of float32. The step between neighbouring values is 0.25 µV or smaller within ±512 µV, 0.5 µV up to ±1,024 µV, and doubles with each further doubling of the value. The largest value is ±65,504 µV (±65.5 mV).
  - Zen 5 (the recommended CPU) converts between float16 and float32 but has no float16 arithmetic, and filters need float32 precision anyway. So the helpers convert every input to float32 and every output back: two conversions at every connection, including connections within one core, where the data is in L1 either way and the smaller size saves nothing.
  - On a ring between cores, float16 moves 2 cache lines per sample instead of 4 at 64 channels, and 63 instead of 125 at 2000 channels.
  - Unmeasured estimate: at 64 channels the conversions cost more time than the saved cache lines save. At 2000 channels the outcome is unknown and needs a benchmark.
  - Out of range: a float32 value beyond ±65,504 µV converts to infinity, not to the largest float16 value. An IIR filter that receives an infinity turns its state into infinity and then NaN, and outputs NaN until the graph is restarted. Neural channels can't reach that value, because their amplifiers clip first: Intan at ±5 mV, Blackrock at ±8 mV, NP1 at ±12 mV at its lowest gain (50). Signals that can reach it:
    - Auxiliary analog inputs (photodiode, accelerometer, analog sync lines) are in the volt range, and 1 V is 1,000,000 µV. Open: do they use the same stream format as neural channels?
    - Neuralynx's Digital Lynx is DC-coupled with a ±132 mV input range (to be checked), so a large electrode DC offset or a saturating artifact can exceed ±65.5 mV.
    - Derived values in µV² (e.g. signal power) exceed 65,504 for any signal above 256 µV (a 500 µV spike squared is 250,000 µV²).
- Does **64 channels first** (see **Principles** in spec.md) apply here too, i.e. choose the format that is fastest at 64 channels?
- The two priorities conflict here: float16 serves the embedded-systems approach (half the bytes in rings and caches), float32 serves scientific accuracy (no overflow to infinity, no rounding to float16 at every connection). Which one wins?

### Heavy processors
- **Heavy processors** (e.g. a decoder) get one or more cores of their own (see **Splittable processors**); that is how "big" processors are isolated. What moving a processor to another core buys depends on where it sits:
  - **On a serial path** (p1 → p2 → p3 → …), it doesn't lower latency, since one sample still passes every step in order, plus two core-to-core hops. It only gives each core its own time budget per sample (pipelining), so do it only when one core can't keep up.
  - **On a parallel branch** (e.g. ripple branch and spike branch both feeding a Trigger), both branches process the same sample at the same time. Latency at the join drops from `a + b` to `max(a, b + 2h)`, where h is one hop. The saving is at most the smaller branch (best when branches cost about the same), minus two hops.
  - It only helps when the branch is on the critical path (the longest path to an output) and costs much more than 2h. A decoder qualifies; a threshold doesn't.
  - Separate outputs on separate cores don't wait for each other's branches.
  - Splitting also gives each branch its own L1/L2. If the branches together don't fit in one core's L2, this removes cache misses and lowers latency even for cheap branches.
  - Hop cost: see **Rings** in spec.md.
  - Costs: one more busy core (heat), and rings take cache on both cores.

### Several chains on one core
- Chains on one core delay each other. A sample that arrives for one chain waits while another chain on the same core does real work. Example: core 1 runs the chains [p1 p2] and [p4 p5 p6] (see the example under **Per-core loop** in spec.md). If sample N−1 arrives from core 2 just after core 1's ring reader checked the ring, p4 gets it only after p1 and p2 have processed sample N+1 on the next pass. A processor that does heavy work only every N samples (e.g. a decoder) would delay a per-sample chain on the same core by its whole computation, which is why heavy processors get a core of their own (see **Heavy processors**).
- Since heavy processors have their own core, the chains that share a core are light ones. When two light chains have work in the same pass, the one later in the processor order waits for the other, at most for the duration of the other chain's work in that pass (e.g. a few hundred ns). The user's processor order (see **Order** under **Per-core loop** in spec.md) decides which chain waits. Yielding and hardware waits do not help here (see **Waiting for data** in spec.md).
- If the delay between chains matters, the user drags one of the chains onto another core in the node editor. The engine does nothing special. Moving a chain costs one more busy core: more heat, and cores are limited (a 9950X3D has 16, one is reserved for the OS, and in the first implementation each input takes one). In the example it adds no ring, since p3 → p4 already crosses cores; if p4 also read p2 directly, that connection would become a ring too.

### Feedback connections
Decided:
- Feedback is mostly a state (e.g. an adaptive threshold, "a stimulation is ongoing", the last stimulation's sample number), passed by reference like any state (see **Buffers** in spec.md). A state keeps its value, so its reader sees it whenever it runs.
- Across cores, a feedback connection is an ordinary ring.
- Within a chain, feedback through a buffer (events or samples) is a known weakness of the engine, not worth more engine work:
  - The reader sees the producer's output from the previous pass (see `out()` under **graph.cpp** in spec.md), so it has seen the result for sample N before it processes N+1.
  - It sees that output during one pass only, and live that pass is usually an empty one. So the reader must read its feedback input even when its other inputs are empty, and copy what it needs (the rule for several inputs, see **Processors** in spec.md).
  - A reader that skips it misses feedback without an error, e.g. the last sample of every 12-sample Neuropixels 1.0 packet.

### Buffer reuse
Decided:
- **Buffer reuse**: the processors in one core's loop share buffers instead of each owning a separate one, to keep the cache footprint small. Example: a loop of 5 processors does not need 5 separate buffers. Since the engine owns the output buffers (see **graph.cpp** in spec.md), this needs no change to any processor.

Open (formerly open question 9): how buffers are assigned, i.e. which connections may share a buffer.
- Fan-out: `input_buf` is read by both `ap` and `lfp` (see the sketch under **graph.cpp** in spec.md), so no processor may overwrite that buffer until both have read it in the current pass.
- Feedback connections hold the previous pass's output (see **Feedback connections**), so that buffer must survive into the next pass and cannot be shared with other connections.
- A buffer can point into a ring slot or a socket receive buffer (see **Buffers** in spec.md). A processor that writes its output into such a buffer in place would corrupt data that other readers of the same ring still need.
- Who decides which connections share a buffer: the generator of `graph.cpp`, which sees the whole graph and the call order on each core, or each processor (e.g. a filter that declares it can work in place)?

### Event rings between cores
- Between cores, events go through a small ring of fixed-size records (see **Events** in spec.md). There is no completeness tracking: a processor that needs complete events (e.g. spike binning) runs on the same core as the processor producing them. The event ring's writer cache line reserves space for a future "processed up to sample N" field, in case it's ever needed.

### Spike records
The prototype's `Spike` holds only the sample number and the channel (see **Events** in spec.md). Richer records, each made by its own processor from the detector's spikes and the filtered stream, so the path from detector to TTL never waits for them:
- the peak amplitude, about 0.1–0.3 ms after the crossing;
- per-group marks for clusterless decoding, e.g. `template <int N> struct GroupSpike : Event { u32 group; float peak[N]; };` (32 B for a tetrode);
- the waveform, e.g. 40 samples per channel of a tetrode (656 B), about 20 samples late (shape 14 in graph_shapes.md).

In all of them, `sample` is the sample number of the threshold crossing, so every record of one spike carries the same number and they can be matched.

### Slow readers
- **Slow readers** (viewer, recorder, audio) subscribe to what the hot loop publishes and never slow down the hot cores. The viewer is just another processor in the graph.
- A slow reader that falls behind (formerly part of the rings question in spec.md): the viewer can skip data, but the recorder must not lose any. Is falling behind a failure for every reader, or only for some?

### Experiment files
- **Each experiment is saved as one plain zip file** (e.g. `my_experiment.zip`) containing `graph.cpp` and every resource it uses.
  - Each recording stores a copy of the experiment zip it was recorded with, so every dataset shows exactly which graph, filters and IMRO produced it (recording itself is discussed later).
  - Resources such as filter and IMRO files can live anywhere the user likes.
  - When the user picks a resource, Falcon copies it into the experiment zip, so later edits, moves or deletes of the original don't affect the experiment.

### Channel labels and positions
- Stream metadata the prototype doesn't need yet: channel labels and positions. To be designed with cache in mind. Open: whether they are compile-time constants like the channel count and the rate (see **Rates** in spec.md), or runtime data that reaches processors in one of these ways:
  - a. passed once at graph start, read-only during streaming
  - b. stored next to the ring, so readers get it when they attach
  - c. sent as a special first record in the stream

### Channel selection
- What happens when the user only wants some channels, e.g. channels 16 to 24, and discards the rest? It touches the stream type (channel count), the padding of channels to a multiple of 16, and where the other channels are dropped (in the input, or in a processor). Not designed yet.

### Rings with many readers
The prototype's rings have one writer and one reader (SPSC, see **Rings** in spec.md). With SPSC, fan-out to several cores writes the same sample into each ring. The alternative for later:
- One writer, many readers (SPMC): one ring per output, and each reader keeps its own read position. Fan-out needs no copies. Since a full ring is a failure, the writer never reads reader positions; each reader detects being overrun from the sample number. Slow readers subscribing to what the hot loop publishes fit this model.

### Ring header layout
The prototype puts each sample's header in its ring slot, in front of the samples (see **Rings** in spec.md). Later optimisation: headers in a separate array next to the slots. Samples fill whole cache lines, so a header in the slot always adds one cache line per sample, while a separate array (two headers per cache line) adds half a line. The saving shrinks as channel counts grow: 3 vs 2.5 lines per sample at 32 channels (17% less), 5 vs 4.5 at 64 (10%), 25 vs 24.5 at 384 (2%), 126 vs 125.5 at 2000 (0.4%). The cost is a little more bookkeeping: two arrays instead of one.

### Ring bundling
- Possible later optimisation: bundle all connections from one core to another into a single ring per direction (one write position to check, fewer shared cache lines). Harder when the bundled connections have different rates or mix events and continuous data.

### Hardware wait
- **hardware wait** (MWAITX on AMD, UMWAIT on Intel): the core sleeps until the ring's write-position cache line is written. No OS involved, caches kept, low power. Added only if a diagnostic on the target machine shows its wake-up time is close to spin.
- A hardware wait watches a single memory address range (one cache line) per core. A core whose loop reads several rings (e.g. one per chain) can sleep on only one of them; data arriving in another ring does not wake the core.

### Huge pages
- Each core's memory (rings, processor state, buffers) in 2 MB pages instead of 4 KB pages, so the CPU needs far fewer address translations (TLB entries). At any channel count; the gain is to be measured.
- Pages reserved at boot, not Linux's transparent huge pages: those are merged by a kernel thread (khugepaged) while the graph runs, which sends TLB-flush interrupts to the realtime cores.
- Engine-only, no processor changes: e.g. each core thread's stack lives in a huge page, so processors and buffers stay local variables of the core's function (see **Per-core loop** in spec.md).

### Vector chains
- Register reuse inside a chain: pass one SIMD vector of channels at a time through consecutive per-channel processors.
  - Values then stay in registers instead of going through L1 at every processor boundary (~2–5 ns per boundary at 384 channels; an estimate, to be measured).
  - Deferred as premature optimisation.
  - Adding it later without touching processors needs per-channel processors written in a per-vector form: a per-sample "do I run?" plus a per-vector `process()` that returns its output instead of writing it.

### Exact replay
Not a goal: a replay should give about the same triggers as the live run (99% matching is enough), and for the same input most processors give exactly the same output anyway (see **Principles** in spec.md). If exact replays are ever needed, e.g. for regression tests, these break exactness:
- logic that depends on CPU time, e.g. a TTL pulse timed in CPU time;
- joins whose result depends on timing, e.g. taking the newest value from another core instead of matching samples by sample number;
- the AVX512 and AVX2 versions of a processor rounding differently, so a value right at a threshold can cross it on one machine and not on another.

### Jitter and joins across rates
- **Jitter from uneven passes** (the pass in which an every-N processor fires takes longer) and **join semantics across rates**: low priority. Joins are the processor's own business.

### Runtime performance measurement
- Not the priority now. Measure, while the graph runs, the time between consecutive processors, including ring hops between cores, using the CPU's timestamp counter (TSC). This would show the user when a chain waits for another chain's work on the same core. Details for later:
  - Cost: each TSC read takes tens of CPU cycles (to be measured), paid at every processor boundary on every pass. Always on, or only in a separate measurement build of `graph.cpp`?
  - Realtime cores never print, so the timestamps would go through a ring to a non-realtime core, like log messages (see **Logging** in spec.md).
  - TSC values from different cores are only comparable if the TSC is invariant and synchronised across cores; to be checked on the target CPUs.
  - Related: the arrival time in each sample's header (see **Buffers** in spec.md) and task A.2 (benchmark each processor on the target machine before the graph runs).

### Scaling to +2000 channels
- Ignored while designing for 64 channels. Notes for the future:
  - A float32 sample is 8 KB (125 cache lines), so every core crossing gets about 5× more expensive.
  - Ring depth competes with cache: 1 ms of ring is 240 KB.
  - Processor state no longer fits L1: one 4-section filter is 64 KB, more than Zen 5's 48 KB.
  - At one sample per pass (see **One sample per pass** in spec.md), such a filter reloads its state from L2 for every sample (estimate: a few hundred ns per filter). Several samples per call would keep a group of channels' state in registers across those samples, but adds latency and means rewriting every processor.
  - The sample width (float16 vs float32, see **Sample width: float16**) halves or doubles all of these.

## UI
- **Live viewer**: the viewer is a processor in the engine and the UI is a separate program, so the viewer's data has to cross from the engine process to the UI process. Not designed yet, and not part of the prototype.
- The UI shows a cache view with per-core L1/L2/L3 usage, so the user sees when the graph is about to spill out of cache (see **Principles** in spec.md, and task D.5).
- **Viewing data without a graph** (UI topic, formerly an open question): maybe the UI runs a small generated engine, e.g. NeuropixelsInput → Viewer.
- **Buffers the cache view can't see** (formerly an open question). Processors keep their own buffers (e.g. p3's copies while it waits for another input). The cache view (task D.5) can only count them if their size is known before the graph runs.

## Tasks
From the former TODO.md. Its other tasks are now part of spec.md.

- **A:**  Refactor Falcon engine
    1. Detect CPU microarch on the fly and change grub settings to isolate all cores but one for Falcon.
    2. Advanced, later: benchmark each processor on the target machine with synthetic data (at the configured channel count), so the UI can show whether each core keeps up before the graph runs. Needs careful design, since other programs running on the machine (e.g. a browser) skew the results.
    3. Advanced, later: track how many slots each ring actually uses during a run, and log a hint when it's far below its size (e.g. only 2 of 64 used → shrink to 4), since oversized rings waste cache.
    4. Check the UDP receive buffer limit (`net.core.rmem_max`) at startup. Debian's default (212992) caps socket buffers at ~416 KiB, only a few ms of Neuropixels data, so any short receiver stall makes the kernel drop packets where Falcon can't see them. If it's below 32 MiB, Falcon asks the user, and on approval sets it immediately and persists it in `/etc/sysctl.d/` (needs root, e.g. via pkexec), so users never set it by hand.
    5. Turn off interrupt batching (coalescing) on the network card that receives acquisition data. Batching makes packets sent 33 µs apart reach Falcon two or three at a time and adds a delay of its own (e.g. `ethtool -C <nic> adaptive-rx off rx-usecs 0`).
    6. Advanced, later: kernel bypass for network inputs (AF_XDP, or DPDK). The card writes packets straight into Falcon's memory with no kernel copy, and checking for a packet is a memory read, so an input can live in a processing core's loop instead of on its own core. AF_XDP can take only the UDP data and leave the TCP control connection to the kernel. Catches: an `electrodePacket` is 10,056 bytes, bigger than a 9,000-byte jumbo frame, so either Falcon reassembles the IP fragments or the sender sends smaller packets. Also, the card's receive ring becomes the buffer that has to prevent packet loss.
- **C:** JIT
    1. Replace existing YAML based graph definition with "poor-mans-JIT".
    2. Ship with clang compiler and compile the graph from the generated .cpp file.
    3. Generate one loop per core in `graph.cpp` (see **Per-core loop** in spec.md).
- **D:** UI
    1. Make scientifically accurate multichannel viewer that never drops a frame and up to modern neuroscience ephys standards. UI should be at least as good as other ephys software, such as Cheetah, SpikeGlx, OpenEphys, Plexon, Trellis and more.
        1. Units and scale: real µV (ADC counts × probe gain), positive-up polarity, µV and ms scale bars.
        2. Trace fidelity: continuous traces between columns, real lines when zoomed in below 1 sample per pixel, saturation/clipping indicator.
        3. Signal conditioning: per-channel offset removal, selectable display filter (e.g. 300 Hz high-pass), optional common average reference, always labelled in the header.
        4. Time integrity: use packet timestamps, show dropped packets as visible gaps, sample rate from the input instead of `30000`.
        5. Navigation and layout: vertical zoom per channel or for all channels, adjustable row height and channels on screen, ordering by probe geometry/depth, hide bad or reference channels.
        6. Display modes: sweep vs scrolling, trigger mode for a stable picture.
        7. Overlays: threshold lines, spike ticks from SpikeDetector, TTL/event markers.
        8. Extra views: probe heatmap (RMS or spike rate vs depth), LFP band view, spike waveform panel.
    2. Minimalize CPU usage to absolute minimum by using OpenGL 4.6.0 and use shaders for rendering waveforms on the screen.
    3. Implement audio output from ephys signal, offer custom signal channel selection to user and merge the selected channels and send them to speaker.
    4. Drop the UI written in Dart, rewrite it in Dear Imgui. Use C++26 reflection to get processors and processor options for drag-and-drop graph node editor.
    5. Cache view. A cache miss costs ~100 ns versus a few ns for a hit, so a graph that outgrows the CPU cache gets much slower without any visible sign. Show per-core L1/L2/L3 usage (ring buffers + processor state on that core, against cache sizes read from the CPU) in the graph editor while the user designs the graph, before running it. Sizes are calculated, not measured, so users see when their graph stops fitting.
    6. Bypass toggle for processors with the same input and output type (signal passes through unchanged). Only while the graph is not running.
    7. Let the user set the order of processors on each core in the node editor (see **Per-core loop** in spec.md).
- **E:** Neuropixels
    1. Until imec provides Linux drivers, probes are run from a separate Windows PC with our own sender program. It uses imec's Windows API to control the probes and streams raw samples to Falcon over Ethernet. Falcon acts as the front end (like OpenEphys): the user manages probes from the Falcon UI, and the sender applies the settings.
    2. Two connections between Falcon and the sender: UDP for data (`electrodePacket`), plain TCP with length-prefixed JSON for control (probe list, IMRO = per-channel electrode site and gain table, start/stop, status).
    3. Samples arrive as raw ADC counts. Falcon converts them to µV and orders channels by depth using the config the sender reports as applied (not the one requested). Config changes are only allowed while streaming is stopped.
    4. IMRO editor in the Falcon UI. NP1 has 960 recording sites on the shank but only 384 channels; each channel N can read site N, N+384 or N+768 (bank 0/1/2), so a channel's number doesn't always match its height on the shank. The editor draws the shank, lets the user mark regions to record from, enforces one site per channel, sets gains, and saves/loads `.imro` files (SpikeGLX format) that are sent to the sender.
    5. Rewrite `src/processors/neuropixels_input.cpp` (the current version is a sketch). It runs alone on its own spinning core, checks the socket without sleeping (no `io_uring_wait_cqe`, no `notify_one()` per packet), and writes each packet's 12 samples into the ring to the processing cores.
