# Core

New Falcon engine, implemented as a single `engine.cpp` in `src/core`. It replaces the old `falcon/` engine (disruptor ring buffer, heap-allocated `TimeSeriesType` packets).

## Goal
One generic multichannel stream type that every acquisition input produces and every processor and the UI viewer consume. Hardware-specific structures like Neuropixels' `electrodePacket` exist only inside their input processor.

## Design principle: everything fits in cache
We fight cache misses. The whole running algorithm, meaning every ring plus every processor's state, must fit in the cache of the core it runs on.
- Hot path: what is touched for every block (rings, processor state such as filter coefficients and history). It is sized against L1/L2 per core and L3 shared.
- Cold data, such as stream metadata, is read only at graph start and never in the hot loop. Processors copy the few values they need (e.g. 384 µV-per-bit scales, 1.5 KB) into their own compact state.
- Ring bytes = blocks × block size × padded channels × bytes per sample. Example, an NP1 ring of 64 blocks × 12 samples × 384 channels:
  - int16: 590 KB, which fits in a 1 MB L2
  - float32: 1.18 MB, which doesn't fit
- The UI shows a cache view with per-core L1/L2/L3 usage, so the user sees when the graph is about to spill out of cache (see TODO.md, UI).

## Decided
- Code lives in `src/core`, as one big `engine.cpp`.
- **Block size is configurable.** The default is the smallest unit the source can deliver:
  - Neuralynx and Ripple: 1 sample, since they deliver each sample immediately
  - Neuropixels 1.0: 12 samples, since its hardware sends 12 AP samples per packet at 2.5 kHz, so 0.4 ms is its latency floor
- **Ring size** is an adjustable number of blocks, always a power of 2.
- **A full ring is a failure** that the engine doesn't handle. The writer never waits, and there is no backpressure. The failure is logged to the console, and the user must change the graph or the hardware.
- **One processor per stream**, never per channel or tetrode (the old engine's hundreds of processors were a major cause of slowness). A processor handles all channels with SIMD, and per-group settings (e.g. per-tetrode thresholds) are arrays inside it.
- **Events** are a second, lightweight connection type: fixed-size records (sample number + payload) in a small ring, used for sparse outputs such as spikes, ripple onsets and decoder results. There is no completeness tracking: a processor that needs complete events (e.g. spike binning) runs on the same core as the processor producing them. The event ring's writer cache line reserves space for a future "processed up to sample N" field, in case it's ever needed.
- **Feedback loops** carry at least one block of delay: the back edge keeps the previous block.
- **Graph definition** is a generated .cpp file (`graph.cpp`), compiled with the bundled clang (JIT plan).
  - The UI re-opens a saved graph by reading `graph.cpp` back, so the file follows a fixed generated format. Node positions are stored as comments at the end of the file; if they are missing, the UI places the nodes itself.
  - The node editor can be simple and fairly static rather than smooth and Figma-like. Adding a processor by clicking it instead of dragging is fine.
- **Each experiment is saved as one plain zip file** (e.g. `my_experiment.zip`) containing `graph.cpp` and every resource it uses.
  - Each recording stores a copy of the experiment zip it was recorded with, so every dataset shows exactly which graph, filters and IMRO produced it (recording itself is discussed later).
- **Only extensions are installed machine-wide** (e.g. a lab's private processors).
  - Resources such as filter and IMRO files can live anywhere user likes to, e.g.
  - When the user picks a resource, Falcon copies it into the experiment zip, so later edits, moves or deletes of the original don't affect the experiment.
- **Filters** are set by high-pass and low-pass values in `graph.cpp`. A coefficient file is needed only for complex filters (e.g. coefficients designed in Python or MATLAB).
- **Extension versioning is the lab's responsibility.** Falcon doesn't track extension versions, since an extension folder may not be a git repo. A missing extension still shows up as a compile error naming its namespace when `graph.cpp` is built.
- **Extensions live in `src/processors/custom/<extension name>/`**, one folder per extension. The folder name is the extension's C++ namespace, so processors from different extensions can share a name without clashing.
  - Falcon ships only the processors directly in `src/processors/`.
  - `custom/` is git-ignored except `custom/template/`: a template extension with a dummy processor that labs copy to start their own.
  - The template is written once the processor interface (open question 4) is decided.
- **Placement** is chosen by the user in a single node editor:
  - CPU cores are drawn as areas in the node editor (one core is reserved for the OS and interrupts, chosen from the detected CPU topology; ideally a core outside the large-L3 CCD, e.g. core 15 on a 9950X3D, so all V-cache cores go to Falcon. Falls back to core 0 if the boot CPU's kernel duties can't be moved; to be checked on preempt_rt).
  - The user places each processor in a core's area and draws connections freely between any processors, also across cores. Each processor ends up with a core number.
- **Chains are detected automatically.** Before JIT, a pass follows the connections: as long as the core number stays the same, processors belong to the same chain, and as soon as it changes, the chain ends. The user never marks chains.
  - JIT compiles each chain's per-block code into one fused unit. There are no rings inside a chain, and each processor keeps its own state.
  - A chain starts at an input (hardware, or a ring from another core) and runs to its end without waiting. A core can have several chains, and it runs whichever one has new input.
  - Writing to a ring (to another core) can happen anywhere in a chain, since writes never wait.
  - Example: p1 → p2 → p3 → p4 → p5 → p6, with p3 on core 2 and the rest on core 1. The core number changes after p2 and after p3, so there are three chains: [p1 p2] and [p4 p5 p6] on core 1, [p3] on core 2. While core 2 works on block N, core 1 already processes block N+1 in the first chain and block N−1 in the second. With p3 on core 1 too, it would be a single chain [p1 … p6].
  - If a processor needs both a local result and a result from another core (e.g. p4 reads p2 and p3), the local result for block N waits in a small buffer in the core's cache until the remote block N arrives.
- **Rings** are inserted only where a connection crosses from one core to another: one ring per connection, in either direction. Two cores can have several rings between them, and each ring counts toward the cache of both the writer's and the reader's core.
- **Heavy processors** (e.g. a decoder) get a core of their own; that is how "big" processors are isolated. What moving a processor to another core buys depends on where it sits:
  - **On a serial path** (p1 → p2 → p3 → …), it doesn't lower latency, since one sample still passes every step in order, plus two core-to-core hops. It only gives each core its own time budget per block (pipelining), so do it only when one core can't keep up.
  - **On a parallel branch** (e.g. ripple branch and spike branch both feeding a Trigger), both branches process the same block at the same time. Latency at the join drops from `a + b` to `max(a, b + 2h)`, where h is one hop. The saving is at most the smaller branch (best when branches cost about the same), minus two hops.
  - It only helps when the branch is on the critical path (the longest path to an output) and costs much more than 2h. A decoder qualifies; a threshold doesn't.
  - Separate outputs on separate cores don't wait for each other's branches.
  - Splitting also gives each branch its own L1/L2. If the branches together don't fit in one core's L2, this removes cache misses and lowers latency even for cheap branches.
  - Hop cost: about 20 ns for the write position within a CCD and about 80 ns across CCDs, plus pulling the block itself, which grows with block size (cheap for a 768 B 1-sample block, more for NP1's 9 KB block; to be measured). Keep connected cores on one CCD.
  - Costs: one more busy core (heat), and rings take cache on both cores.
- Only processors placed on a core are written to the generated `graph.cpp`; everything else does not run. Empty cores stay idle, which lowers heat.
- **Waiting for data**: the user chooses the wait strategy per core:
  - **spin**: lowest latency (one cache-line transfer), caches stay warm, core at 100% (heat). For realtime cores.
  - **blocking**: the OS wakes the thread (µs), and deep idle (C6) can flush L1/L2. For non-realtime cores (recorder, viewer).
  - **hardware wait** (MWAITX on AMD, UMWAIT on Intel): the core sleeps until the ring's write-position cache line is written. No OS involved, caches kept, low power. Added only if a diagnostic on the target machine shows its wake-up time is close to spin.
  - Falcon 1's yielding and sleeping (`falcon/portpolicy.hpp`) are dropped. On an isolated core with one thread, yielding is just spinning plus syscalls, and sleeping backs off to 1 ms (30 sample periods at 30 kHz).
- **Start/stop** applies to the whole graph at once, never per processor.
- **Config changes** happen only while the graph is not running, for all processors.
- **Errors** are logged to the console.

## Graph shapes to support
Reference list of graph shapes. The engine design (core placement, rings, joins) must handle all of them. The graph is not a DAG: feedback loops (7) are supported.

| # | Shape | Example |
|---|---|---|
| 1 | **Chain** | Input → Filter → SpikeDetector → Output |
| 2 | **Fan-out** | Input → {AP filter, LFP filter, Recorder, Viewer} |
| 3 | **Diamond** (split, then rejoin, same source) | Input → ripple branch + spike branch → Trigger |
| 4 | **Join at different rates** | Decoder (every 10 ms) + RippleDetector (every sample) → Trigger |
| 5 | **Multiple sources, different clocks** | Camera position tracking + acquisition system → Decoder (not a priority now) |
| 6 | **Rate change** | 30 kHz → 1 kHz LFP decimation; continuous → events (detector); events → continuous (spike counts per bin) |
| 7 | **Feedback loop** | Stimulation times fed back to blank the stim artifact at the input; adaptive threshold |
| 8 | **Non-realtime sinks** | Recorder, viewer, audio: must never slow the realtime path |
| 9 | **Channel split/merge** | One probe split by shank or region into separate branches, merged later |
| 10 | **Bursty heavy compute** | Decoder runs a big computation every N ms, idle in between |
| 11 | **Multiple outputs** | Several stimulators or TTL lines driven by different branches |
| 12 | **Disconnected subgraphs** | Two independent experiments in one graph |

## Open questions
Each question lists candidate answers.

1. **Sample type between processors**
   - a. Raw int16 + per-channel µV-per-bit scale: half the memory traffic, but every processor must apply the scale.
   - b. float32 µV: simplest processors, inputs convert once, 2× memory traffic.
   - c. Per stream: the description declares the type, and processors declare what they accept.
2. **Stream metadata** (channel count, sample rate, µV per bit for each channel, channel labels and positions)
   - Do we need a separate per-stream description at all, and what goes in it? To be designed with cache in mind.
   - If yes, how it reaches processors:
     - a. passed once at graph start, read-only during streaming
     - b. stored next to the ring, so readers get it when they attach
     - c. sent as a special first block in the stream
3. **Blocks and rings**
   - Header fields:
     - a. minimal: first sample number, hardware timestamp, flags
     - b. a. plus ingestion CPU timestamp (TSC) for latency measurement
   - Ring structure (only for connections between cores):
     - a. one writer, one reader (SPSC; TODO A.2 currently says SPSC): one ring per connection. Simplest, fewest shared cache lines, but fan-out to several cores writes the same block into each ring.
     - b. one writer, many readers (SPMC): one ring per output, and each reader keeps its own read position. Fan-out needs no copies. Since a full ring is a failure, the writer never has to read reader positions; each reader detects being overrun from the block's sample number.
     - Possible later optimisation: bundle all connections from one core to another into a single ring per direction (one write position to check, fewer shared cache lines). Harder when the bundled connections have different rates or mix events and continuous data.
   - Payload layout:
     - a. sample-major (all channels of sample 0, then sample 1, …): SIMD across channels, 16 per AVX512 instruction. Matches NP1's packet layout (`apData[12][384]`).
     - b. channel-major (all samples of channel 0, then channel 1, …): SIMD across time within one channel.
     - With 1-sample blocks the two layouts are identical, so the choice only matters for multi-sample blocks (e.g. NP1's 12).
4. **Processor interface**
   - a. A plain struct with a `process(block in, block out)` function, plus member fields as options, read by C++26 reflection for the UI.
   - b. A base class with virtual functions, as in the old engine.
   - c. Concepts/templates: compile-time checked and no virtual calls; fits the JIT plan.
