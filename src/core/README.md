# Core

New Falcon engine, implemented as a single `engine.cpp` in `src/core`. It replaces the old `falcon/` engine (disruptor ring buffer, heap-allocated `TimeSeriesType` packets).

## Goal
One generic multichannel stream type that every acquisition input produces and every processor and the UI viewer consume. Hardware-specific structures like Neuropixels' `electrodePacket` exist only inside their input processor.

## Top priorities
These two priorities guide every design decision about the sample format between processors and about the graph as a whole:
- **Scientific accuracy**: every value a processor receives or outputs is the correct physical quantity (e.g. µV), with no precision loss, overflow or rounding that could change a detection or a trigger without an error message.
- **Embedded-systems approach**: the data the hot loop touches stays in the caches of the core that uses it (see **Design principle: everything fits in cache**), and values stay in CPU registers from one processing step to the next instead of being written to memory and read back (see **Vector chains** under Deferred).

## Design principle: everything fits in cache
We fight cache misses. The whole running algorithm, meaning every ring plus every processor's state, must fit in the cache of the core it runs on.
- Hot path: what is touched for every sample (rings, processor state such as filter coefficients and history). It is sized against L1/L2 per core and L3 shared.
- Cold data, such as stream metadata, is read only at graph start and never in the hot loop. Processors copy the few values they need (e.g. the input processor's 384 µV-per-bit scales, 1.5 KB) into their own compact state.
- Ring bytes = samples × padded channels × bytes per sample.
  - One float32 sample is 256 B (4 cache lines) at 64 channels and 1.5 KB at 384 channels (NP1).
  - A ring of 64 samples × 384 channels is 48 KB as int16 and 96 KB as float32.
- The UI shows a cache view with per-core L1/L2/L3 usage, so the user sees when the graph is about to spill out of cache (see TODO.md, UI).

## Design principle: every sample as soon as possible
- Every sample is processed as soon as it reaches Falcon: block size 1 wherever the hardware allows, and the smallest buffers everywhere.
- Losing packets is not acceptable. Buffers start as small as possible; if packets get lost, the user makes the buffer bigger.
- Replaying a recording must give the same triggers as the live run.

## Decided

### Engine model
- **UI and engine are separate programs.** The UI is the frontend. It generates `graph.cpp`, compiles it with the bundled clang into an engine executable, and runs that as a child process. Compile time is not a concern (seconds with a build cache on a 9950X); runtime performance is.
- **One sample per run.** A chain runs once per time point, across all channels; there are no multi-sample blocks. This is where nanosecond-level optimisation pays off, and imec may provide Linux drivers that deliver single samples. Until then, a 12-sample NP1 packet is processed as 12 runs back to back.
- **Processors** are stateful or stateless. In Falcon 1, each processor ran its own `while (running)` loop in its own thread. Here, `process()` has no loop. It checks its input first and returns at once if the input is empty, then does its work. No `process()` ever waits for data, because a waiting processor would stall every other processor on its core. All logic lives inside the processor, including when it runs:
  - A processor that produces output only every N samples (decimator, spike-count binner, decoder) leaves its output empty in between, so everything downstream of it also runs only every N samples.
  - A processor with several inputs decides itself how to combine them.
  - Phases become state. Falcon 1's RippleDetector had a burn-in loop before its detection loop; here it is `if (burningIn) { update statistics; return; }`.

**`graph.cpp`** stays as simple as this sketch. It holds the processors with their options, a ring wherever a connection crosses cores, and one loop per core. Each loop calls its processors in upstream-first order, so a sample goes from input to output in a single pass.

```cpp
Ring lfp_to_ripple, ripple_to_trigger;     // inserted where a connection crosses cores

void core2() {
    NeuropixelsInput   input   {.port = 6363};
    Filter             ap      {.highpass = 300, .lowpass = 6000};
    SpikeDetector      spikes  {.threshold = -50};
    Decimator          lfp     {.factor = 30};
    Trigger            trigger {};
    PCIeParallelOutput ttl     {};

    while (running) {
        input.process();                   // polls the hardware
        ap.process(input.out);             // every process() returns at once if its input is empty
        spikes.process(ap.out);
        lfp.process(input.out);            // has output only every 30th sample
        lfp_to_ripple.push(lfp.out);
        trigger.process(spikes.out, ripple_to_trigger);
        ttl.process(trigger.out);
    }
}

void core3() {
    RippleDetector ripple {.threshold = 3};

    while (running) {
        ripple.process(lfp_to_ripple);
        ripple_to_trigger.push(ripple.out);
    }
}
```

- **Feedback loops** carry at least one sample of delay. Inside a core, a processor that reads an output written later in the loop sees the previous pass.
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

- **Slow readers** (viewer, recorder, audio) subscribe to what the hot loop publishes and never slow down the hot cores. The viewer is just another processor in the graph.
- **Logging**: errors and messages go to a logger on a separate core. Processors reach it through a non-blocking ring, with no system calls in the hot loop.
- **Hardware input/output latency** is outside the engine design. It is being worked out with the neurotech companies, who shape their firmware for it.
- **Channel count**: design and optimise for 64 channels now. 2000 channels come next year, with a year to refactor. At any channel count, packets must never be dropped.

### Graph, placement and rings
- Code lives in `src/core`, as one big `engine.cpp`.
- **A full ring is a failure** that the engine doesn't handle. The writer never waits, and there is no backpressure. The failure is logged to the console, and the user must change the graph or the hardware.
- **One processor per stream**, never per channel or tetrode (the old engine's hundreds of processors were a major cause of slowness). A processor handles all channels with SIMD, and per-group settings (e.g. per-tetrode thresholds) are arrays inside it.
- **Events** are a second, lightweight connection type: fixed-size records (sample number + payload) in a small ring, used for sparse outputs such as spikes, ripple onsets and decoder results. There is no completeness tracking: a processor that needs complete events (e.g. spike binning) runs on the same core as the processor producing them. The event ring's writer cache line reserves space for a future "processed up to sample N" field, in case it's ever needed.
- **Block size is configurable.** The default is the smallest unit the source can deliver:
  - Neuralynx and Ripple: 1 sample, since they deliver each sample immediately
  - Neuropixels 1.0: 12 samples, since its hardware sends 12 AP samples per packet at 2.5 kHz, so 0.4 ms is its latency floor
- **Ring size** is an adjustable number of blocks, always a power of 2.
- **A full data ring is a failure** that the engine doesn't handle. The writer never waits, and there is no backpressure. The failure is logged to the console, and the user must change the graph or the hardware. Log rings are the exception: a full log ring drops and counts messages (see **Logging**).
- **One processor per stream**, never per channel or tetrode (the old engine's hundreds of processors were a major cause of slowness). A processor handles all channels with SIMD, and per-group settings (e.g. per-tetrode thresholds) are arrays inside it.
- **Events** are a second, lightweight connection type: fixed-size records (sample number + payload) in a small ring, used for sparse outputs such as spikes, ripple onsets and decoder results. There is no completeness tracking: a processor that needs complete events (e.g. spike binning) runs on the same core as the processor producing them. The event ring's writer cache line reserves space for a future "processed up to sample N" field, in case it's ever needed. Within one core, a processor hands over the events it produced on the current pass as an event view (see **One loop per core**); event rings are only used between cores.
- **Feedback connections** are one pass late. In the generated loop, a feedback connection is a variable declared outside the loop that holds the previous pass's result. Like any view, it exists for one pass only, so the processor that reads it copies it if it needs it later.
- **Graph definition** is a generated .cpp file (`graph.cpp`), compiled with the bundled clang (JIT plan).
  - The UI re-opens a saved graph by reading `graph.cpp` back, so the file follows a fixed generated format. Node positions are stored as comments at the end of the file; if they are missing, the UI places the nodes itself.
  - The node editor can be simple and fairly static rather than smooth and Figma-like. Adding a processor by clicking it instead of dragging is fine.
- **Each experiment is saved as one plain zip file** (e.g. `my_experiment.zip`) containing `graph.cpp` and every resource it uses.
  - Each recording stores a copy of the experiment zip it was recorded with, so every dataset shows exactly which graph, filters and IMRO produced it (recording itself is discussed later).
- **Only extensions are installed machine-wide** (e.g. a lab's private processors).
  - Resources such as filter and IMRO files can live anywhere user likes to, e.g.
  - When the user picks a resource, Falcon copies it into the experiment zip, so later edits, moves or deletes of the original don't affect the experiment.
- **Processor params are** are either primitives string, int, double or a string path to a file such as filter coefficients file or imro file.
- **Extension versioning is the lab's responsibility.** Falcon doesn't track extension versions, since an extension folder may not be a git repo. A missing extension still shows up as a compile error naming its namespace when `graph.cpp` is built.
- **Extensions live in `src/processors/custom/<extension name>/`**, one folder per extension. The folder name is the extension's C++ namespace, so processors from different extensions can share a name without clashing.
  - Falcon ships only the processors directly in `src/processors/`.
  - `custom/` is git-ignored except `custom/template/`: a template extension with a dummy processor that labs copy to start their own.
  - The template is written once the processor interface (open question 5) is decided.
- **Placement** is chosen by the user in a single node editor:
  - CPU cores are drawn as areas in the node editor (one core is reserved for the OS and interrupts, chosen from the detected CPU topology; ideally a core outside the large-L3 CCD, e.g. core 15 on a 9950X3D, so all V-cache cores go to Falcon. Falls back to core 0 if the boot CPU's kernel duties can't be moved; to be checked on preempt_rt).
  - The user places each processor in a core's area and draws connections freely between any processors, also across cores. Each processor ends up with a core number.
- **Chains are detected automatically.** Before JIT, a pass follows the connections: as long as the core number stays the same, processors belong to the same chain, and as soon as it changes, the chain ends. The user never marks chains.
  - In `graph.cpp` a chain is just consecutive calls in its core's loop. There are no rings inside a chain, and each processor keeps its own state.
  - Every processor is a concrete type in one translation unit, so the compiler can inline a whole core loop into one fused unit.
  - A chain starts at an input (hardware, or a ring from another core) and runs to its end without waiting. A core can have several chains: its loop calls all of them, and processors without new input return at once.
  - Chains on one core delay each other. A block that arrives for one chain waits while another chain on the same core does real work. Example: core 1 runs the chains [p1 p2] and [p4 p5 p6] (see the example below). If block N−1 arrives from core 2 just after core 1's ring reader checked the ring, p4 gets it only after p1 and p2 have processed block N+1 on the next pass. A processor that does heavy work only every N samples (e.g. a decoder) would delay a per-sample chain on the same core by its whole computation, which is why heavy processors get a core of their own (see **Heavy processors**).
  - Within one chain there is no such delay: a block passes through every processor of the chain in the same pass, and an empty pass costs only one empty-input check per processor (estimate: under 1 ns each, to be measured).
  - Since heavy processors have their own core, the chains that share a core are light ones. When two light chains have work in the same pass, the one later in the processor order waits for the other, at most for the duration of the other chain's work in that pass (e.g. a few hundred ns). The user's processor order (see **Order** under One loop per core) decides which chain waits. Yielding and hardware waits do not help here (see **Waiting for data**).
  - If the delay between chains matters, the user drags one of the chains onto another core in the node editor. The engine does nothing special. Moving a chain costs one more busy core: more heat, and cores are limited (a 9950X3D has 16, one is reserved for the OS, and in the first implementation each input takes one). In the example it adds no ring, since p3 → p4 already crosses cores; if p4 also read p2 directly, that connection would become a ring too.
  - Writing to a ring (to another core) can happen anywhere in a chain, since writes never wait.
  - Example: p1 → p2 → p3 → p4 → p5 → p6, with p3 on core 2 and the rest on core 1. The core number changes after p2 and after p3, so there are three chains: [p1 p2] and [p4 p5 p6] on core 1, [p3] on core 2. While core 2 works on sample N, core 1 already processes sample N+1 in the first chain and sample N−1 in the second. With p3 on core 1 too, it would be a single chain [p1 … p6].
  - If a processor needs both a local result and a result from another core (e.g. p4 reads p2 and p3), it decides itself how to combine them, like any processor with several inputs.
- **Rings** are inserted only where a connection crosses from one core to another: one ring per connection, in either direction. Two cores can have several rings between them, and each ring counts toward the cache of both the writer's and the reader's core.
- **Heavy processors** (e.g. a decoder) get one or more cores of their own (see splittable processors); that is how "big" processors are isolated. What moving a processor to another core buys depends on where it sits:
  - **On a serial path** (p1 → p2 → p3 → …), it doesn't lower latency, since one sample still passes every step in order, plus two core-to-core hops. It only gives each core its own time budget per sample (pipelining), so do it only when one core can't keep up.
  - **On a parallel branch** (e.g. ripple branch and spike branch both feeding a Trigger), both branches process the same sample at the same time. Latency at the join drops from `a + b` to `max(a, b + 2h)`, where h is one hop. The saving is at most the smaller branch (best when branches cost about the same), minus two hops.
- **One loop per core.** The JIT generates one loop for each core. Each pass calls every processor placed on that core once, in the order the user chose. Connections are the call arguments, so the compiler sees the whole data flow and can inline every `process()`:
  ```cpp
  while (running) {
      auto r1 = p1.process();
      auto r2 = p2.process();
      auto r3 = p3.process(r1, r2);   // connections p1 → p3 and p2 → p3
      auto r4 = p4.process(r3);
  }
  ```
  - Realtime cores spin, so passes also run when nothing has arrived. A processor that acts on time (e.g. turn a TTL off 1 ms later) checks the time itself on every pass. Whether it counts CPU time or samples is up to the processor.
  - **Outputs are views**: a pointer plus a size, pointing at wherever the data already is (a receive buffer, a ring slot, the producer's own buffer). Nothing is copied to hand data over. An empty view means nothing new on this pass. Small results (e.g. a detector's yes/no) are plain values and can stay in registers.
  - A view of a continuous stream is always exactly one block. After a burst, the extra blocks wait where they already are (in the ring or the socket buffer) and are handed over one per pass. An event view holds the events produced on this pass (zero or more).
  - A view is only valid during the current pass, because the producer reuses its buffer on its next pass. A processor copies what it needs to keep and never keeps the view itself.
  - **Buffer reuse**: the processors in one core's loop share block buffers instead of each owning a separate one, to keep the cache footprint small. Example: a loop of 5 processors does not need 5 separate block buffers. How buffers are assigned is open question 9.
  - **Each processor decides when it works.** It looks at its inputs on every pass and silently returns an empty output when it isn't ready. The engine never buffers anything for a processor. Example: p3 needs blocks with the same sample number from p1 and p2. If p1's block N arrives first, p3 copies it into its own buffer and waits; when p2's block N arrives, p3 processes both. If an input stops, p3 drops its oldest kept block and logs a warning once.
  - **Order**: the user sets the order of processors on each core. A processor must come after the processors it reads from. If it comes before one of them, that connection becomes a feedback connection (one pass late).
  - Writing to a ring (to another core) can happen anywhere in the loop, since writes never wait.
  - Example: p1 → p2 → p3 → p4 → p5 → p6, with p3 on core 2 and the rest on core 1. Core 1's loop holds p1, p2, the ring writer to core 2, the ring reader from core 2, and p4, p5, p6. On one pass it can handle block N+1 in p1 and p2 and block N−1 in p4 to p6, while core 2 works on block N. With p3 on core 1 too, all six run in one loop with no rings.
  - Joins across cores: if p4 reads p2 (same core) and p3 (another core), p4 itself keeps p2's block N until p3's block N arrives through the ring.
- **Rings** are inserted only where a connection crosses from one core to another: one ring per connection, in either direction. Two cores can have several rings between them, and each ring counts toward the cache of both the writer's and the reader's core.
  - On the reading core, a ring reader works like any processor in the loop: its output is the next block, or an empty view if nothing new has arrived. Checking it is a memory read.
  - **A stuck processor does not stall other cores.** Example (the p1–p6 example above): p3 on core 2 stops making progress (a long computation, a bug, a CPU stall). Core 1 keeps running: its ring reader for p3 → p4 returns an empty view, so p4, p5 and p6 return at once, and p1 and p2 keep processing new blocks. The consequences:
    - p4, p5 and p6 produce no output while p3 is stuck.
    - The ring p2 → p3 fills up, and a full ring is a failure (see **A full data ring is a failure**). Example: a 64-block ring of 1-sample blocks at 30 kHz is full after 2.1 ms.
    - When p3 recovers, the blocks waiting in the ring reach p3 and then p4 one per pass (see **Outputs are views**), so p4 to p6 process late blocks until p3 has caught up. Catching up requires p3 to process blocks faster than they arrive.
- **Input processors (first implementation)** are each placed alone on their own spinning core and hand blocks to the processing cores through a ring. That costs one core-to-core hop per block, but the socket's system calls and kernel copies stay off the processing cores, so their caches stay warm. This is a placement, not an engine rule: with kernel bypass (TODO.md, A) an input can later move into a processing core's loop.
- **Heavy processors** (e.g. a decoder) get a core of their own; that is how "big" processors are isolated. What moving a processor to another core buys depends on where it sits:
  - **On a serial path** (p1 → p2 → p3 → …), it doesn't lower latency, since one sample still passes every step in order, plus two core-to-core hops. It only gives each core its own time budget per block (pipelining), so do it only when one core can't keep up.
  - **On a parallel branch** (e.g. ripple branch and spike branch both feeding a Trigger), both branches process the same block at the same time. Latency at the join drops from `a + b` to `max(a, b + 2h)`, where h is one hop. The saving is at most the smaller branch (best when branches cost about the same), minus two hops.
  - It only helps when the branch is on the critical path (the longest path to an output) and costs much more than 2h. A decoder qualifies; a threshold doesn't.
  - Separate outputs on separate cores don't wait for each other's branches.
  - Splitting also gives each branch its own L1/L2. If the branches together don't fit in one core's L2, this removes cache misses and lowers latency even for cheap branches.
  - Hop cost: about 20 ns for the write position within a CCD and about 80 ns across CCDs, plus pulling the sample itself (4 cache lines for 64 float32 channels, 24 for 384; to be measured). Keep connected cores on one CCD.
  - Costs: one more busy core (heat), and rings take cache on both cores.
- Only processors placed on a core are written to the generated `graph.cpp`; everything else does not run. Empty cores stay idle, which lowers heat.
- **Waiting for data**: realtime cores always spin. Only non-realtime cores may block.
  - **spin**: lowest latency (one cache-line transfer), caches stay warm, core at 100% (heat). For realtime cores.
  - **blocking**: the OS wakes the thread (µs), and deep idle (C6) can flush L1/L2. For non-realtime cores (recorder, viewer, logger).
  - **hardware wait** (MWAITX on AMD, UMWAIT on Intel): the core sleeps until the ring's write-position cache line is written. No OS involved, caches kept, low power. Added only if a diagnostic on the target machine shows its wake-up time is close to spin.
  - A core that sleeps (blocking or hardware wait) runs no empty passes, so processors that act on time (e.g. turning a TTL off) belong on spinning cores.
  - A hardware wait watches a single memory address range (one cache line) per core. A core whose loop reads several rings (e.g. one per chain) can sleep on only one of them; data arriving in another ring does not wake the core.
  - The waiting strategy only decides what a core does when none of its processors has new input. It does not change the delay between chains on one core when both have work in the same pass.
  - Falcon 1's yielding and sleeping (`falcon/portpolicy.hpp`) are dropped. On an isolated core with one thread, yielding is just spinning plus syscalls, and sleeping backs off to 1 ms (30 sample periods at 30 kHz).
- **Start/stop** applies to the whole graph at once, through one start/stop signal, never per processor.
- **Config changes** happen only while the graph is not running, for all processors.
- **Logging**: a logger runs on a non-realtime core. Realtime cores never print; they send log messages to the logger through a ring.
  - The realtime core writes only a message number and the raw values, and the logger turns them into text.
  - A full log ring is not a failure: the message is dropped and counted, and the logger reports how many were dropped.
  - A realtime core never wakes the logger. The logger checks its rings on its own schedule (e.g. every few ms).
  - Processors warn once instead of on every pass (e.g. once when an input stops).
  - Errors, such as a full data ring, are logged the same way.

### Sample type
- **Input processors convert every sample to µV.** Whatever the acquisition system, every processor after the input receives samples in µV, in one common floating-point format (float32 or float16, see open question 2). No processor other than the input ever does math on raw ADC counts. Device-specific details (integer formats, per-channel gains, offsets, units) are handled only inside the input processor.
  - Acquisition systems deliver different formats (not yet verified against the vendors' documentation): int16 raw ADC counts (Neuropixels, Blackrock), int32 raw ADC counts holding 24-bit values (Neuralynx raw UDP packets), floating point in µV (Ripple), floating point in volts (TDT). So "raw" is not one integer type, and "float" is not one unit.
  - Converting in the input prevents two bugs that would give wrong results without any error message:
    - **Mixed gains**: on Neuropixels 1.0 the IMRO table sets the gain of each channel separately, so the same raw count means a different voltage on different channels, and on different probes. Any processor that combines channels (common average reference, bipolar referencing, a decoder comparing spike amplitudes across channels) would compute wrong values from raw counts.
    - **Headroom**: NP1 samples are 10-bit values stored in int16 (range −512 to 511), while Blackrock samples use the full int16 range. Integer math that has room to add or subtract on NP1 data overflows on Blackrock data.
  - Cost: input processors run alone on their own core (see **Input processors** under Graph, placement and rings), so in every graph the samples pass through a ring right after the input. With the conversion in the input, that ring carries floats instead of int16: with float32, 4 cache lines per sample instead of 2 at 64 channels, and 125 instead of 63 at 2000 channels. The conversion itself is about 3 AVX512 instructions per 16 channels (widen to 32-bit, convert to float, multiply by the per-channel scale).

### Rates
- **Two rate levels.** The engine supports a hot path, which is the stream at the acquisition system's sample rate (e.g. 30 kHz), and a slower path at a lower rate (e.g. 1 kHz). No planned pipeline uses the slower path yet, but the engine design must support it. What distinguishes the slower path from the hot path besides its rate is open question 8.

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
| 13 | **Multiple sources, same clock** | 2–4 Neuropixels probes → one decoder; their packets arrive at slightly different times |
| 14 | **Join after a delayed branch** | A spike waveform extractor (needs ~20 samples after the peak) joined with a branch without that delay; blocks are matched by sample number |

## Open questions
Each question lists candidate answers where there are any.
Each question lists candidate answers where we have them.

1. **Connections** (next topic)
   - What a connection carries: a sample (sample type, channel count, rate) or events.
   - What "empty" means:
     - A local output can be read by several processors (e.g. `input.out` by both `ap` and `lfp`), so a reader can't clear it. One option: an output holds only what its processor produced in the current pass, and the processor clears it on its next call.
     - For a ring, empty means this reader has read everything.
     - Both should look the same to the processor.
   - How a connection inside a core differs from a ring between cores.
2. **Sample width** (in discussion). Decided: input processors convert to µV (see **Sample type**). Open: the floating-point format of a sample, float32 or float16.
   - Value range the format must hold: spikes stay within about ±500 µV, LFP can reach a few mV, and a stimulation artifact drives the ADC to its full range (e.g. ±8 mV on Blackrock).
   - a. **float32**: processors read and write float32 directly, with no conversions. 32 bits is more than the signal needs: acquisition ADCs deliver 10 to 24 bits, and the noise floor limits the useful resolution further.
   - b. **float16** (IEEE half precision): half the bytes of float32. The step between neighbouring values is 0.25 µV or smaller for values within ±512 µV, 0.5 µV up to ±1,024 µV, and doubles with each further doubling of the value. The largest value is ±65,504 µV (±65.5 mV).
     - Zen 5 (the recommended CPU) has instructions to convert between float16 and float32 but no float16 arithmetic, and filters need float32 precision anyway. So each processor converts its float16 input to float32, computes in float32, and converts its output back to float16. That is two conversions at every connection between processors, including connections within one core, where the data is in L1 either way and the smaller size saves nothing.
     - On a ring between cores it moves 2 cache lines per sample instead of 4 at 64 channels, and 63 instead of 125 at 2000 channels.
     - Unmeasured estimate: at 64 channels the conversions cost more time than the saved cache lines save. At 2000 channels the outcome is unknown and needs a benchmark.
     - Out of range: converting a float32 value beyond ±65,504 µV to float16 gives infinity, not the largest float16 value. An IIR filter that receives an infinity turns its state into infinity and then NaN, and outputs NaN until the graph is restarted. Neural channels of the systems below cannot reach that value, because their amplifiers clip first: Intan at ±5 mV, Blackrock at ±8 mV, NP1 at ±12 mV at its lowest gain (50). Signals that can reach it:
       - Auxiliary analog inputs (photodiode, accelerometer, analog sync lines) carry values in the volt range, and 1 V is 1,000,000 µV. Open: do they use the same stream format as neural channels, or the slower path (question 8)?
       - Neuralynx's Digital Lynx: DC-coupled with a ±132 mV input range (to be checked), so a large DC offset of an electrode or a saturating artifact can exceed ±65.5 mV.
       - Derived values in µV² (e.g. signal power) exceed 65,504 for any signal above 256 µV (a 500 µV spike squared is 250,000 µV²).
   - **Channel count** says to design for 64 channels now and refactor for 2000 channels next year. Does that also apply here, i.e. choose the format that is fastest at 64 channels now? Changing the format later changes the input and output type of every processor, including processors that labs wrote in their own extensions.
   - The two top priorities conflict here: float16 serves the embedded-systems priority (half the bytes in rings and caches), float32 serves scientific accuracy (no overflow to infinity, no rounding to float16 at every connection). Which priority wins when they conflict?
3. **Stream metadata** (channel count, sample rate, channel labels and positions). The µV-per-bit scales are no longer stream metadata: they stay inside the input processor (see **Sample type**).
   - Do we need a separate per-stream description at all, and what goes in it? To be designed with cache in mind. Since compile time is free, the stream's shape (sample type, channel count, rate) could be compile-time constants in `graph.cpp`.
   - If yes, how it reaches processors:
     - a. passed once at graph start, read-only during streaming
     - b. stored next to the ring, so readers get it when they attach
     - c. sent as a special first record in the stream
4. **Rings**
   - Header fields per sample:
     - a. minimal: sample number, hardware timestamp, flags
     - b. a. plus ingestion CPU timestamp (TSC) for latency measurement
     - Either way, every block carries its first sample number: processors match blocks from different inputs by it.
   - Ring structure (only for connections between cores):
     - a. one writer, one reader (SPSC; TODO A.2 currently says SPSC): one ring per connection. Simplest, fewest shared cache lines, but fan-out to several cores writes the same sample into each ring.
     - b. one writer, many readers (SPMC): one ring per output, and each reader keeps its own read position. Fan-out needs no copies. Since a full ring is a failure, the writer never has to read reader positions; each reader detects being overrun from the sample number. Slow readers subscribing to what the hot loop publishes fit this model.
     - Possible later optimisation: bundle all connections from one core to another into a single ring per direction (one write position to check, fewer shared cache lines). Harder when the bundled connections have different rates or mix events and continuous data.
   - Ring size: an adjustable number of samples, a power of 2 (as decided earlier for blocks)?
   - A slow reader that falls behind: the viewer can skip data, but the recorder must not lose any. Is falling behind a failure for every reader, or only for some?
   - Payload layout is no longer a question: with one sample per run, a sample is all channels side by side.
5. **Processor interface**
   - Decided so far: the `process()` shape above, and `delegate(i)` for splittable processors.
   - a. A plain struct with a `process()` function, plus member fields as options, read by C++26 reflection for the UI.
   - b. A base class with virtual functions, as in the old engine.
   - c. Concepts/templates: compile-time checked and no virtual calls; fits the JIT plan.
6. **Splittable processors**
   - How many parts are allowed: decide once the data passed through the pipeline is clearer.
   - Which parts read the input:
     - a. only the original part, which hands out the work (one extra core crossing)
     - b. every part directly: each part notices new data itself, so only collecting the results costs a crossing
   - Can a part's core run other processors too? It works, but the original part waits for the slowest part, so other work on those cores delays the processor.
   - A final name for `delegate(i)` (`help` was rejected).
7. **Viewing data without a graph** (UI topic): maybe the UI runs a small generated engine, e.g. NeuropixelsInput → Viewer.
8. **Slower path** (in discussion, see **Rates**)
   - Besides its rate, what is different about the slower path? Candidates: its own sample format, its own cores, its own loop separate from the hot path's loop, a lower priority. Note that the engine already handles lower rates inside the hot path: a processor that outputs only every N samples (e.g. a decimator) makes every processor downstream of it run only every N samples (see **Processors** under Engine model).
   - Exactly two rates, or any number of rates? Rates that occur in practice: NP1 LFP band 2.5 kHz, decimated LFP 1 kHz, decoder time bins 100 Hz (10 ms), camera 30–60 Hz.
   - The hot path is not always 30 kHz: Neuralynx samples at 32 kHz, TDT at about 24.4 kHz. So the hot path's rate must come from the input processor, not be fixed at 30 kHz.
   - A slower stream from the same device has a rate that is an exact integer fraction of the hot path's rate (NP1: 30 kHz / 12 = 2.5 kHz), so its samples line up with hot path samples. A stream from another device (e.g. a camera) runs on its own clock and drifts relative to the hot path (shape 5).
   - Do auxiliary analog inputs (photodiode, accelerometer) go on the slower path? See question 2: in µV, their volt-range values overflow float16.
9. **Buffer reuse: how buffers are assigned.** Decided: buffers are reused within a core's loop (see **Buffer reuse** under One loop per core). Open: which connections may share a buffer.
   - Fan-out: `input.out` is read by both `ap` and `lfp` (see the `graph.cpp` sketch), so no processor may overwrite that buffer until both have read it in the current pass.
   - Feedback connections hold the previous pass's output (see **Feedback connections**), so that buffer must survive into the next pass and cannot be shared with other connections.
   - A view can point into a ring slot or a socket receive buffer (see **Outputs are views**). A processor that writes its output into such a buffer in place would corrupt data that other readers of the same ring still need.
   - Who decides which connections share a buffer: the generator of `graph.cpp`, which sees the whole graph and the call order on each core, or each processor (e.g. a filter that declares it can work in place)?
10. **To confirm**: proposed during discussion, not yet agreed.
   - Each core's processors are declared inside that core's function (as in the sketch), so processors of different cores never share a cache line (false sharing). Rings are then the only memory that cores share.
   - Processors never hardcode the channel count. It is a compile-time parameter, so the same code runs at 64 and at 2000 channels.
   - Flush-to-zero is set on every core. Flat or zeroed channels (dead, reference, or blanked during stimulation) drive IIR filter state into denormal floats, which many CPUs process far more slowly.
   - Event outputs and event rings have room for every channel firing in the same sample. A stimulation artifact crosses threshold on all channels at once, and since a full ring is a failure, a too-small ring would end the run.
   - Voltage streams are in µV, not V. Floating point has the same relative precision at any magnitude, so precision favours neither unit; µV is chosen because users enter thresholds and read traces in µV.
   - Samples are floating point, not integers in whole µV. Rounding the raw signal to whole µV would be harmless, since 1 µV is below the noise floor of extracellular recordings (several µV RMS). But after filtering, a frequency band can have an amplitude of only a few µV, so rounding to whole µV becomes a large relative error. Phases (radians) and z-scores are fractions. And every processor would have to round and clamp its output; a processor that forgets to clamp makes the integer overflow without an error.
   - Every continuous stream uses the same floating-point format (open question 2), whatever its unit. Voltage streams are in µV; other streams carry other units, e.g. a phase estimator outputs radians and a spike binner outputs spike counts per bin.

## Deferred
- **Vector chains** (register reuse inside a chain): pass one SIMD vector of channels at a time through consecutive per-channel processors.
  - Values then stay in registers instead of going through L1 at every processor boundary (~2–5 ns per boundary at 384 channels; an estimate, to be measured).
  - Deferred as premature optimisation.
  - Adding it later without touching processors needs per-channel processors written in a per-vector form: a per-sample "do I run?" plus a per-vector `process()` that returns its output instead of writing it.
- **Jitter from uneven runs** (the pass in which an every-N processor fires takes longer) and **join semantics across rates**: low priority. Joins are the processor's own business.
- **Runtime performance measurement** (not the priority now): measure, while the graph runs, the time between consecutive processors, including ring hops between cores, using the CPU's timestamp counter (TSC). This would show the user when a chain waits for another chain's work on the same core. Details for later:
  - Cost: each TSC read takes tens of CPU cycles (to be measured), paid at every processor boundary on every pass. Always on, or only in a separate measurement build of `graph.cpp`?
  - Realtime cores never print, so the timestamps would go through a ring to a non-realtime core, like log messages (see **Logging**).
  - TSC values from different cores are only comparable if the TSC is invariant and synchronised across cores; to be checked on the target CPUs.
  - Related: open question 4 (a TSC timestamp in each ring block's header) and TODO.md A.4 (benchmark each processor on the target machine before the graph runs).
- **Scaling to 2000 channels** is ignored while designing for 64. Notes for next year:
  - A float32 sample is 8 KB (125 cache lines), so every core crossing gets about 5× more expensive.
  - Ring depth competes with cache: 1 ms of ring is 240 KB.
  - Processor state no longer fits L1: one 4-section filter is 64 KB, more than Zen 5's 48 KB.
  - 2000 channels usually means several devices, possibly with separate clocks, so combining them is a join across sources (shape 5).
  - The sample width (float16 vs float32, open question 2) halves or doubles all of these.
  - Use huge pages to avoid TLB misses.
   - Payload layout:
     - a. sample-major (all channels of sample 0, then sample 1, …): SIMD across channels, 16 per AVX512 instruction. Matches NP1's packet layout (`apData[12][384]`).
     - b. channel-major (all samples of channel 0, then channel 1, …): SIMD across time within one channel.
     - With 1-sample blocks the two layouts are identical, so the choice only matters for multi-sample blocks (e.g. NP1's 12).
4. **Processor interface**
   - How `process()` is called is decided (see **One loop per core**): the generated loop passes the upstream views as arguments and uses the return value as the output. Still open is how a processor type is written:
   - a. A plain struct with a `process(...)` function, plus member fields as options, read by C++26 reflection for the UI.
   - b. A base class with virtual functions, as in the old engine.
   - c. Concepts/templates: compile-time checked and no virtual calls; fits the JIT plan.
5. **Rules that fail silently.** A processor author must copy what they need to keep and never keep a view. Breaking either rule gives wrong triggers and no error. Keeping a view compiles without a warning, and when it breaks depends on where the view points: a producer's own buffer is overwritten on the next pass, so the bug shows at once; a ring slot is overwritten only when the ring wraps around (e.g. 64 samples later), so a short test passes. The same processor can therefore work on one core layout and give wrong values on another.
6. **Buffers the cache view can't see.** Processors keep their own buffers (e.g. p3's copies while it waits for another input). The cache view (TODO.md, D.5) can only count them if their size is known before the graph runs.
7. **Same triggers on replay.** This depends on each processor's own logic (e.g. matching blocks by sample number vs. taking the newest value), which the engine can't see.
