# Prototype spec

The new Falcon engine is a single `engine.cpp` in `src/core`. It replaces the old `falcon/` engine (disruptor ring buffer, heap-allocated `TimeSeriesType` packets).

This file is the minimum spec for the prototype. Later work is in [beyond_prototype.md](beyond_prototype.md). The graph shapes the engine must support are in [graph_shapes.md](graph_shapes.md).

## Goal
One generic multichannel stream type. Every input processor produces it; every other processor and the UI viewer consume it. Hardware-specific structures (e.g. Neuropixels' `electrodePacket`) exist only inside their input processor.

## Priorities
These two guide every design decision, from the sample format to the graph as a whole:
- **Scientific accuracy**: every value a processor receives or outputs is the correct physical quantity (e.g. µV). No precision loss, overflow or rounding may change a detection or a trigger without an error message.
- **Embedded-systems approach**: the data the hot loop touches stays in the caches of the core that uses it (see **Principles**), and values stay in CPU registers from one processing step to the next instead of being written to memory and read back (see **Vector chains** in beyond_prototype.md).

## Principles
- **Everything fits in cache.** All rings and all processor state on a core must fit in that core's cache.
  - Hot data is what is touched for every sample: rings, and processor state such as filter coefficients and history. It is sized against L1/L2 per core and the shared L3.
  - Cold data, such as stream metadata, is decided on compile time. Processors can have the few values they need into their own compact state (e.g. an input's 384 µV-per-bit scales, 1.5 KB).
  - Channels are padded to a multiple of 16, one AVX512 vector of float32, so a sample fills whole 64 B cache lines and SIMD loops have no leftover channels.
  - Ring bytes = samples × padded channels × bytes per sample. One float32 sample is 256 B (4 cache lines) at 64 channels and 1.5 KB at 384 channels (NP1). A 64-sample ring at 384 channels is 48 KB as int16 and 96 KB as float32.
- **Every sample as soon as possible**: a buffer size of 1 by default (see **Buffer size**), and the smallest rings and buffers everywhere.
- **No lost packets.** Rings and buffers start as small as possible. If packets get lost, the user makes them bigger: a bigger ring survives a short stall (see **Rings**), and a bigger buffer size lets a graph that is too slow keep up (see **Buffer size**).
- **Deterministic replay**: replaying a recording should give about the same triggers as the live run; 99% matching is enough. For the same input, most processors give exactly the same output anyway, so no extra effort goes into making replays exactly identical (see **Exact replay** in beyond_prototype.md).
- **Low channel count first** Design and optimise for ~32-64 channels now. +2000 channels come next year, with a year to refactor (see **Scaling to 2000 channels** in beyond_prototype.md).

## Prototype plan
Goal: a first engine that runs a real graph end to end. The basics are defined, and later optimisations stay possible.

### Agreed
- **No generator.** We write `graph.cpp` by hand to test the engine. In the product, users never write it: the UI generates it from the processor nodes they drag and drop (see **Generator** in beyond_prototype.md).
- **Input: Ripple Neuromed, 32 channels.** To verify against Ripple's documentation:
  - the sample format (listed as floating point in µV under **Sample type**);
  - how many samples each packet carries (the default **Buffer size** of 1 assumes Ripple delivers each sample immediately);
  - the device timestamp each packet carries (see **Rings**);
  - the rate of its raw stream (30 kHz expected), which goes into `graph.cpp` as a compile-time constant (see **Rates**);
  - that its data arrives over UDP (see **Machine setup**).
- **Graph**: the Ripple input alone on its own core (see **Input processors**), a ring to one processing core that runs Filter → SpikeDetector → TTL output, the start/stop signal, and a minimal logger. This is shape 1 (chain) of graph_shapes.md. The processor interface must not rule out any other shape.
- **Machine setup** by hand: one core handles the OS and interrupts and all other cores are isolated for Falcon; interrupt batching is turned off on the network card that receives Ripple's data; the UDP receive buffer limit is raised so a short stall doesn't drop packets in the kernel. Automating these is tasks A.1, A.4 and A.5 in beyond_prototype.md.
- **No live viewer.** The path for the viewer's data from the engine process to the UI process is not needed yet (see **Live viewer** in beyond_prototype.md).
- **How we judge that it works**:
  - Correct triggers: Ripple's software (Trellis) records the same session alongside Falcon. A reference script applies the same filter and threshold to that recording. At least 99% of the triggers Falcon logged must match the sample numbers it detects.
  - Latency inside Falcon: for each trigger, the engine logs the time from packet arrival to the TTL write, using the CPU's timestamp counter (TSC). The arrival time is read on the input core and the TTL time on the processing core, so the TSC must be invariant and synchronised across cores; to be checked on the target CPU.
  - Latency end to end: the TTL output is wired back into a digital input of the Ripple system, so the trigger is recorded on the same clock as the neural data.
- **How to prioritise**: decide now what is expensive to change later, and build the simplest version of everything else.
  - Expensive: what processor code sees, i.e. the `process()` signature, the buffer type and the sample type. Every processor depends on it, including the ones labs write in their own extensions.
  - Cheap: what only the engine sees, i.e. rings, threads, buffer assignment and the waiting strategy. These can be optimised later without touching any processor, as long as processors only ever see buffers, never rings.

## Decided

### Boundaries
- **UI and engine are separate programs.** In the product, the UI generates, compiles and runs the engine (see **Generator** in beyond_prototype.md). Compile time is not a concern (seconds with a build cache on a 9950X); runtime performance is.
- **Hardware input/output latency** is outside the engine design. It is being worked out with the neurotech companies, who shape their firmware for it.

### Buffer size
The number of samples each processor handles per call, adjustable by the user like the buffer size in audio software. Not to be confused with the ring size (see **Rings**). The default is 1: each pass processes one time point across all channels. This is where nanosecond-level optimisation pays off.
- With a buffer size of N, every processor runs once per N samples, e.g. 7,500 times per second instead of 30,000 at a buffer size of 4. Per-call costs (input checks, loading state, the ring hop between cores) are paid N times less often, which lets a slower CPU keep up without losing packets. The math per sample stays the same: filtering 4 samples is still 4 times the work of filtering 1.
- A bigger buffer size adds latency, because a buffer waits until it has N samples.
- **`process()` receives the whole buffer** and loops over its samples itself, like an audio callback. Calling `process()` once per sample instead would keep every processor at 30,000 calls per second and defeat the purpose.
- **Compile-time constant**, like the channel count. At a buffer size of 1, the compiler removes each processor's loop over the buffer. Changing the buffer size means recompiling `graph.cpp`.
- **Buffer layout**: sample-major for now, i.e. all channels of sample 0, then all channels of sample 1, and so on. SIMD works across channels (16 per AVX512 instruction), and it matches NP1's packet layout (`apData[12][384]`). Letting processors choose the layout is in beyond_prototype.md (**Buffer layout preference**).
- The rest of this spec assumes a buffer size of 1 unless it says otherwise. What is still open above 1 is under **Buffer size above 1** in beyond_prototype.md.
- Neuralynx and Ripple are expected to deliver each sample immediately (for Ripple, to verify; see **Agreed**). Neuropixels 1.0 sends 12 AP samples per packet at 2.5 kHz, so 0.4 ms is its latency floor. imec may provide Linux drivers that deliver single samples; until then, at a buffer size of 1, a 12-sample NP1 packet is processed in 12 passes back to back.

### Processors
- **One processor per stream**, never per channel or tetrode (Falcon 1's hundreds of processors were a major cause of slowness). A processor handles all channels with handwritten SIMD: every processor has a hand-crafted AVX512 version and a hand-crafted AVX2 fallback, rather than relying on the compiler to vectorize. The version for the machine is picked at compile time, since `graph.cpp` is compiled for the machine that runs it. There is no scalar code, and AVX2 is required to run Falcon. Per-group settings (e.g. per-tetrode thresholds) are arrays inside the processor.
- **`process()` never loops and never waits.** In Falcon 1, each processor ran its own `while (running)` loop in its own thread. Here, `process()` checks its inputs, returns at once if there is nothing to do, and otherwise does its work. A waiting processor would stall every other processor on its core.
- **All logic lives inside the processor**, including when it runs. The engine never buffers anything for a processor.
  - A processor that produces output only every N samples (decimator, spike-count binner, decoder) leaves its output empty in between, so everything downstream of it also runs only every N samples. This is how lower rates exist inside a graph.
  - A processor with several inputs combines them itself. Example: p3 needs sample N from both p1 and p2. If p1's sample N arrives first, p3 copies it into its own state and waits; when p2's sample N arrives, p3 processes both. It works the same when p2 is on another core and its samples arrive through a ring. If an input stops, p3 drops its oldest kept sample and logs a warning once.
  - Phases become state. Falcon 1's RippleDetector had a burn-in loop before its detection loop; here it is `if (burningIn) { update statistics; return; }`.
  - A processor that acts on time checks the time itself on every pass, counting either CPU time or samples. Anything that ends an output, such as turning a TTL off 1 ms after a trigger, is timed in CPU time, so it ends even if the input stops. Such a processor must run on a spinning core (see **Waiting for data**).
- **A processor is a plain struct** (for now): a `process()` function plus public fields, so `graph.cpp` sets its options with designated initializers (e.g. `Filter ap {.highpass = 300, .lowpass = 6000}`). All fields are public, including internal state such as filter history. No virtual functions: they would force one `process()` signature on every processor and rule out designated initializers. Later, C++26 reflection can read the option fields and `process()`'s parameters (its inputs and outputs) for the UI (other options for later: **Processor type alternatives** in beyond_prototype.md).
- **The channel count is a compile-time constant**, so the compiler knows every loop's length and every buffer's size (e.g. 32 float32 channels are exactly two AVX512 vectors, or four AVX2 vectors). A processor never hardcodes it: the same code runs at 32, 64 or 2000 channels, and different streams can have different counts (e.g. the branches of a channel split, shape 9 in graph_shapes.md). A new channel count means regenerating and recompiling `graph.cpp`.
- **Stream type**: the compile-time facts about a stream (channel count, sample rate, sample type, buffer size) are grouped in one named type, and each processor takes it as a template parameter:
  ```cpp
  using RippleRaw = Stream<32, 30000, float, 1>;   // written once

  Filter<RippleRaw>        ap     {.highpass = 300};
  SpikeDetector<RippleRaw> spikes {.threshold = -50};
  ```
  Adding a fact later changes only `Stream`, not any processor. A processor whose output stream differs from its input takes both stream types and derives what it needs from them: a decimator from 30 kHz to 1 kHz gets its factor of 30 from the two types and needs no option for it. The generator writes these types; for the prototype we write them by hand.
- **Options** (params) are primitives (string, int, double) or a path to a file, such as a filter coefficients file or an IMRO file.
- Processors are stateful or stateless. Each is a concrete type in one translation unit, so the compiler can inline a whole core loop into one fused unit.
- **Setup**: a plain struct can't have a constructor, so the engine calls a setup function on each processor before the first pass. It runs on the core that will run the processor, so the processor's state starts out in that core's cache.

### graph.cpp
`graph.cpp` holds the processors with their options, their buffers, a ring wherever a connection crosses cores, and one loop per core.

**In/out buffers.** `process()` reads its input buffers, writes its output buffers and returns nothing, like an audio plugin. `graph.cpp` declares every buffer and passes the buffers as arguments, so the data flow is visible in the loop. Chosen for clarity: there is one way to hand over data.
- As fast as returning values: each buffer is a local variable of its core's function, touched only by the inlined loop (see **Processors**), so the compiler can keep the buffer's count in a register. The samples go through L1 either way. This holds only while buffers stay local to their core's function.
- A processor with several outputs takes several output buffers.
- The engine owns the buffers, so the generator can later let processors share buffers, or let a filter overwrite its input in place, without touching any processor (see **Buffer reuse** in beyond_prototype.md). The UI's cache view can count every buffer from `graph.cpp` (task D.5 in beyond_prototype.md).
- **No copies across rings**: a ring can hand a producer its next free slot as the output buffer, so the producer writes straight into the ring, as the input does in the sketch. A ring reader points its output buffer at the slot it reads.
- **`out()` marks an output**: in the loop, every output buffer of a processor call is wrapped in `out()`, e.g. `lfp.process(raw_buf, out(lfp_buf));`. `out()` clears the buffer (count 0) right before the call, so a processor that returns at once leaves its output empty, and no author can forget to clear it. Clearing right before the producer, not at the start of the pass, keeps feedback connections working, since they read the previous pass's output. It costs one write per buffer per pass, and it shows the direction of the data in the loop.

The sketch is an example with more processors than the prototype's graph. Its ring function names are placeholders, and stream types (see **Stream type** under Processors) are left out for brevity.

```cpp
Ring input_to_core2, lfp_to_ripple, ripple_to_trigger;   // inserted where a connection crosses cores

void core1() {
    NeuropixelsInput input {.port = 6363};

    while (running) {
        Buffer& slot = input_to_core2.next_slot();  // the input writes straight into the ring
        input.process(out(slot));                   // polls the hardware
        input_to_core2.publish(slot);               // makes the samples visible to core 2, if there are any
    }
}

void core2() {
    Filter             ap      {.highpass = 300, .lowpass = 6000};
    SpikeDetector      spikes  {.threshold = -50};
    Decimator          lfp     {};                  // factor 30 comes from its two stream types
    Trigger            trigger {};
    PCIeParallelOutput ttl     {};
    Buffer             input_buf, ap_buf, spikes_buf, lfp_buf, ripple_buf;   // owned by the engine
    bool               ttl_high;                    // a state: keeps its value until trigger changes it

    while (running) {
        input_to_core2.read(input_buf);                     // points input_buf at the next ring slot, or leaves it empty
        ap.process(input_buf, out(ap_buf));                 // out() clears ap_buf right before the call
        spikes.process(ap_buf, out(spikes_buf));            // every process() returns at once if its input is empty
        lfp.process(input_buf, out(lfp_buf));               // has output only every 30th sample
        lfp_to_ripple.write(lfp_buf);                       // copies into the ring
        ripple_to_trigger.read(ripple_buf);
        trigger.process(spikes_buf, ripple_buf, ttl_high);  // ttl_high is a state, so not cleared
        ttl.process(ttl_high);
    }
}

void core3() {
    RippleDetector ripple {.threshold = 3};
    Buffer         lfp_buf, ripple_buf;

    while (running) {
        lfp_to_ripple.read(lfp_buf);
        ripple.process(lfp_buf, out(ripple_buf));
        ripple_to_trigger.write(ripple_buf);
    }
}
```

### Per-core loop
- **One loop per core.** Each core runs one thread, pinned to that core, and each pass of its loop calls every processor on that core once.
- **Each core's processors are declared inside that core's function** (as in the sketch under **graph.cpp**), so processors of different cores never share a cache line (false sharing). Rings are then the only memory that cores share.
- **Flush-to-zero** is set on every core. Flat or zeroed channels (dead, reference, or blanked during stimulation) drive IIR filter state into denormal floats, which many CPUs process far more slowly. Values that small (below 10⁻³⁸) mean nothing for neural data.
- **Order**: processors are called in a fixed order, and each comes after the processors it reads from, so a sample goes from input to output in a single pass. If a processor comes before one it reads from, that connection becomes a feedback connection, one pass late (see **Feedback connections** in beyond_prototype.md). In the product, the user sets the order in the node editor.
- **Chains**: connected processors on the same core form a chain. The chain ends where the next processor is on another core.
  - A chain has no rings inside, and each processor keeps its own state.
  - A chain starts at an input (hardware, or a ring from another core) and runs to its end without waiting: a sample passes through the whole chain in one pass. An empty pass costs one empty-input check per processor (estimate: under 1 ns each, to be measured).
  - A core can run several chains. They delay each other (see **Several chains on one core** in beyond_prototype.md).
- **Ring writes** can happen anywhere in the loop, since a write never waits.
- **Example**: p1 → p2 → p3 → p4 → p5 → p6, with p3 on core 2 and the rest on core 1.
  - Chains: [p1 p2] and [p4 p5 p6] on core 1, [p3] on core 2.
  - Core 1's loop: p1, p2, the ring writer to core 2, the ring reader from core 2, p4, p5, p6.
  - In one pass, core 1 handles sample N+1 in p1 and p2 and sample N−1 in p4 to p6, while core 2 works on sample N.
  - With p3 on core 1 too, all six form one chain in one loop, with no rings.

### Buffers
- A connection carries either samples (all channels of one time point; defined by sample type, channel count and rate) or events (see **Events**).
- **A buffer is a pointer to data plus a count**: up to **Buffer size** samples, or the events produced on this pass (zero or more). Its storage belongs to the engine, but a producer can point it at data that already exists elsewhere (a ring slot, a socket receive buffer), so nothing is copied to hand data over.
- A count of 0 means nothing new on this pass.
- After a burst, the extra samples wait where they already are (in the ring or the socket buffer) and are handed over one buffer per pass.
- Small results that describe a state, such as "the TTL is high" or "a ripple is ongoing", are plain values passed by reference. They keep their value until their producer changes it, and stay in registers once inlined. Anything that means "new on this pass", such as a detection, goes in a buffer, so `out()` clears it (see **graph.cpp**).
- A buffer's contents are valid only during the current pass, because its producer overwrites them on its next pass. Several processors can read the same buffer in that pass (e.g. `input_buf` is read by both `ap` and `lfp` in the sketch). A processor copies what it needs to keep and never keeps a pointer into a buffer.
- A buffer filled by a ring reader looks the same as one filled by a processor on the same core, so a processor can't tell whether its input crossed cores.

### Events
A second, lightweight connection type: fixed-size records (sample number + payload) for sparse outputs such as spikes, ripple onsets and decoder results. Within one core they are handed over in event buffers. Between cores they go through event rings (see **Event rings between cores** in beyond_prototype.md). An event buffer has room for every channel firing in the same sample: a stimulation artifact crosses threshold on all channels at once, and a buffer too small for that would lose events. The same will hold for event rings.

### Rings
- A ring is inserted only where a connection crosses from one core to another: one ring per connection, in either direction. Two cores can have several rings between them. Each ring counts toward the cache of both the writer's and the reader's core.
- **Size**: an adjustable number of samples, always a power of 2.
- **One writer, one reader (SPSC)**, the simplest version: the prototype's only ring has one writer and one reader, and SPSC shares the fewest cache lines between the two cores. The old TODO list planned an SPSC design that minimizes cache misses. One writer with many readers is in beyond_prototype.md (**Rings with many readers**).
- **Each sample's header** holds three values, about 24 B:
  - its sample number, which processors use to match samples from different inputs;
  - the device's own timestamp for that sample, which links Falcon's samples to the device's recording (e.g. Trellis, for the correctness check under **Agreed**);
  - its arrival time (TSC), for the latency check under **Agreed**. The prototype's chain takes a sample from the ring to the TTL in one pass, so the engine logs the latency as the TTL write time minus the arrival time of the sample read in that pass.
  - Falcon 1 recorded the same two times (arrival time and device timestamp). Flags wait until something needs them.
- **The header sits in the slot**, in front of the samples: the simplest layout. Since samples fill whole cache lines, the header adds one cache line per sample (3 instead of 2 at 32 channels). Headers in a separate array would save half a line per sample, a later optimisation (see **Ring header layout** in beyond_prototype.md).
- **A full data ring is a failure.** The writer never waits, and there is no backpressure. The failure is logged, and the user must change the graph or the hardware. Log rings are the exception (see **Logging**).
- **Reading**: on the reading core, a ring reader works like any processor in the loop. It points its output buffer at the next slot, or leaves it empty (count 0) if this reader has read everything. Checking it is a memory read.
- **Hop cost**: about 20 ns for the write position within a CCD and about 80 ns across CCDs, plus pulling the slot itself, header included (3 cache lines at 32 float32 channels, 5 at 64, 25 at 384; to be measured). Keep connected cores on one CCD.
- **A stuck processor does not stall other cores.** In the example under **Per-core loop**, p3 on core 2 stops making progress (a long computation, a bug, a CPU stall):
  - Core 1 keeps running. Its ring reader for p3 → p4 leaves its buffer empty, so p4, p5 and p6 produce no output, while p1 and p2 keep processing new samples.
  - The ring p2 → p3 fills up, and a full ring is a failure. A 64-sample ring at 30 kHz is full after 2.1 ms.
  - When p3 recovers, the waiting samples reach p3 and then p4 one per pass, so p4 to p6 process late samples until p3 has caught up. Catching up requires p3 to process samples faster than they arrive.

### Input processors
Each input processor runs alone on its own spinning core and hands samples to the processing cores through a ring, writing them straight into the ring's slots (see **graph.cpp**). That costs one core-to-core hop per sample, but the socket's system calls and kernel copies stay off the processing cores, so their caches stay warm. This is a placement for the first implementation, not an engine rule: with kernel bypass (task A.6 in beyond_prototype.md), an input can later move into a processing core's loop.

### Waiting for data
- **Realtime cores spin**, so their loops also run passes when nothing has arrived. Lowest latency (one cache-line transfer), caches stay warm, and the core runs at 100% (heat).
- **Non-realtime cores** (recorder, viewer, logger) may block: the OS wakes the thread (µs), and deep idle (C6) can flush L1/L2. A blocking core runs no empty passes.
- The waiting strategy only decides what a core does when none of its processors has new input. It does not change the delay between chains on one core when both have work in the same pass.
- Falcon 1's yielding and sleeping (`falcon/portpolicy.hpp`) are dropped. On an isolated core with one thread, yielding is just spinning plus system calls, and sleeping backs off to 1 ms (30 sample periods at 30 kHz).

### Start, stop and config
- One start/stop signal for the whole graph, never per processor.
- Config changes happen only while the graph is stopped, for all processors.

### Logging
- A logger runs on a non-realtime core. Realtime cores never print and make no system calls; they send log messages to the logger through a ring.
- A message is a message number plus the raw values. The logger turns it into text.
- A full log ring is not a failure: the message is dropped and counted, and the logger reports how many were dropped.
- A realtime core never wakes the logger. The logger checks its rings on its own schedule (e.g. every few ms).
- Processors warn once instead of on every pass (e.g. once when an input stops).
- Errors, such as a full data ring, are logged the same way.

### Sample type
Input processors convert every sample to µV. Every processor after the input receives µV in one common floating-point format, float32 in the prototype, and never does math on raw ADC counts. Device-specific details (integer formats, per-channel gains, offsets, units) stay inside the input processor.
- **Load and store helpers**: processors read and write samples as SIMD vectors through the buffer's `load` and `store` helpers, never through raw float pointers, and always do their math in float32 registers. The sample type is part of the stream type, fixed at compile time, so the helpers inline to the instructions one would write by hand: plain loads and stores for float32 (`vmovups`). For float16 they would convert on load and store (`vcvtph2ps`, `vcvtps2ph`), which a hand-written float16 processor on Zen 5 has to do anyway, since Zen 5 has no float16 arithmetic. So switching to float16 later changes only the stream type, not any processor (see **Sample width: float16** in beyond_prototype.md). A processor that needs a trick for one format uses a compile-time branch (`if constexpr`).
- **One format for every continuous stream**, whatever its unit (see **Goal**): voltage streams are in µV, and other streams carry other units, e.g. a phase estimator outputs radians and a spike binner outputs spike counts per bin.
- **Why µV, not V**: floating point has the same relative precision at any magnitude, so precision favours neither unit. µV is chosen because users enter thresholds and read traces in µV.
- **Why floating point, not integers in whole µV**: rounding the raw signal to whole µV would be harmless, since 1 µV is below the noise floor of extracellular recordings (several µV RMS). But after filtering, a frequency band can have an amplitude of only a few µV, so rounding to whole µV becomes a large relative error. Phases (radians) and z-scores are fractions. And every processor would have to round and clamp its output; a processor that forgets to clamp makes the integer overflow without an error.
- Devices deliver different formats (not yet verified against the vendors' documentation): int16 raw ADC counts (Neuropixels, Blackrock), int32 raw ADC counts holding 24-bit values (Neuralynx raw UDP packets), floating point in µV (Ripple), floating point in volts (TDT). So "raw" is not one integer type, and "float" is not one unit.
- Converting in the input prevents two bugs that give wrong results without an error message:
  - **Mixed gains**: on Neuropixels 1.0 the IMRO table sets each channel's gain, so the same raw count means a different voltage on different channels and on different probes. Any processor that combines channels (common average reference, bipolar referencing, a decoder comparing spike amplitudes across channels) would compute wrong values from raw counts.
  - **Headroom**: NP1 samples are 10-bit values stored in int16 (−512 to 511), while Blackrock samples use the full int16 range. Integer math that has room on NP1 data overflows on Blackrock data.
- Cost: the ring right after the input (every graph has one, see **Input processors**) carries floats instead of int16. With float32, that is 4 cache lines per sample instead of 2 at 64 channels, and 125 instead of 63 at 2000 channels. The conversion itself is about 3 AVX512 instructions per 16 channels (widen to 32-bit, convert to float, multiply by the per-channel scale).

### Rates
- The sample rate comes from the input processor, and the rest of the pipeline follows it. Nothing assumes 30 kHz: Neuralynx samples at 32 kHz, TDT at about 24.4 kHz.
- **Compile-time constant**, like the channel count. The UI and the generator infer each stream's rate while the user designs the graph, including the lower rate after a decimator, and `graph.cpp` hands it to the processors. In the prototype's hand-written `graph.cpp`, we write it in ourselves.
- Processors use the rate to turn the user's Hz and ms into per-sample values: filter coefficients, and durations as sample counts (refractory period, decoder time bins). Without it, the same graph on other hardware would shift every cutoff and duration without an error: a 300 Hz high-pass designed for 30 kHz sits at 320 Hz on 32 kHz data, and 30 samples are 1 ms at 30 kHz but 0.94 ms at 32 kHz.
- At start, the input processor checks that the device really runs at the compiled rate. If it doesn't, the input logs an error and the graph doesn't start.
- Lower rates inside a graph come from processors that output only every N samples (see **Processors**).

## Open questions
None for the prototype. What is still to verify is listed under **Agreed** (Ripple's documentation, Trellis recording alongside Falcon, the TSC across cores).
