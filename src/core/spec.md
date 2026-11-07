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
- **Every sample as soon as possible**: one sample per pass (see **One sample per pass**), and the smallest rings everywhere.
- **No lost packets.** Rings start as small as possible. If packets get lost, the user makes them bigger: a bigger ring survives a short stall (see **Rings**). A graph too slow to keep up is spread over more cores (see **Heavy processors** in beyond_prototype.md).
- **Deterministic replay**: replaying a recording should give about the same triggers as the live run; 99% matching is enough. For the same input, most processors give exactly the same output anyway, so no extra effort goes into making replays exactly identical (see **Exact replay** in beyond_prototype.md).
- **Low channel count first** Design and optimise for ~32-64 channels now. +2000 channels come in the future (see **Scaling to +2000 channels** in beyond_prototype.md).

## Prototype plan
Goal: a first engine that runs a real graph end to end. The basics are defined, and later optimisations stay possible.

### Agreed
- **No generator.** We write `graph.cpp` by hand to test the engine. In the product, users never write it: the UI generates it from the processor nodes they drag and drop (see **Generator** in beyond_prototype.md).
- **Input: Ripple Neuromed, 32 channels**, read through Ripple's C library (`xl_cont_raw` in xipplib). Its documentation confirms: the raw stream is 30 kHz (a compile-time constant in `graph.cpp`, see **Rates**), electrode data is float in µV, each call returns as many points as requested plus one 32-bit timestamp for the first point. There is no separate sample number: the timestamp counts 30 kHz ticks, one per sample, so the input widens it to 64 bits (32 bits wrap after about 40 hours) and uses it as the sample number (see **Buffers**). Still to verify:
  - how soon a point is available through the library after the hardware sends it (one every 33 µs is expected, see **One sample per pass**);
  - that its data arrives over UDP (see **Machine setup**), and whether the library runs threads of its own.
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
  - Outside the loop: anything that runs outside the per-core loops (setup, teardown, failure handling) costs no latency, so it gets the simplest solution that works, never an elaborate one.

## Decided

### Boundaries
- **UI and engine are separate programs.** In the product, the user designs the graph in the UI, the generator writes `graph.cpp`, clang compiles it into the engine program, and the UI spawns that program (see **Generator** in beyond_prototype.md). Compilation time is not a concern (takes seconds with a build cache on a 9950X); runtime performance is.
- **The bundled clang supports C++26.** Falcon relies on reflection (see **Events** and **Processors**), plus the C++20 features that put `Options` in a template argument: floating-point values and designated initializers there (see **Processors**). A compiler implements the standard feature by feature, so `-std=c++26` alone doesn't guarantee these; check each one on the clang version chosen for bundling.
- **Hardware input/output latency** is outside the engine design. It is being worked out with the neurotech companies, who shape their firmware for it.

### One sample per pass
Each `process()` call handles at most one sample: one time point across all channels. There is no buffer size (samples per call, as in audio software). Batching samples only pays off when each call has a large fixed cost, such as Falcon 1's scheduling of hundreds of OS threads. Here each core runs one pinned thread, so batching would only add latency.
- Neuralynx and Ripple are expected to deliver each sample immediately (for Ripple, to verify; see **Agreed**).
- Neuropixels 1.0 sends 12 samples per packet, one packet every 0.4 ms, so 0.4 ms is its latency floor. Its sample rate is still 30 kHz: the 12 samples go through the graph one per pass, back to back (see **Buffers**), so a spike in the packet's 3rd sample triggers after 3 passes, not after all 12. If imec ever delivers single samples, nothing in the engine changes.
- Adding batching later means rewriting every processor (see **Scaling to +2000 channels** in beyond_prototype.md).

### Processors
- **One processor per stream**, never per channel or tetrode (Falcon 1's hundreds of processors were a major cause of slowness). A processor handles all channels with handwritten SIMD: every processor has a hand-crafted AVX512 version and a hand-crafted AVX2 fallback, rather than relying on the compiler to vectorize. The version for the machine is picked at compile time, since `graph.cpp` is compiled for the machine that runs it. Both versions sit in the same `process()`, in the two branches of `if constexpr (avx512)`, where `avx512` is a constant the engine sets from the compiler's target; `load(v)` then returns an `__m512` or an `__m256` (see **Operations a processor uses** under **Buffers**). There is no scalar code, and AVX2 is required to run Falcon. Per-group settings (e.g. per-tetrode thresholds) are arrays inside the processor.
- **`process()` never loops and never waits.** In Falcon 1, each processor ran its own `while (running)` loop in its own thread. Here, `process()` checks its inputs, returns at once if there is nothing to do, and otherwise does its work. A waiting processor would stall every other processor on its core.
- **All logic lives inside the processor**, including when it runs. The engine never buffers anything for a processor.
  - A processor that produces output only every N samples (decimator, spike-count binner, decoder) leaves its output empty in between, so everything downstream of it also runs only every N samples. This is how lower rates exist inside a graph.
  - A processor with several inputs combines them itself. Example: p3 needs sample N from both p1 and p2. If p1's sample N arrives first, p3 copies it into its own state and waits; when p2's sample N arrives, p3 processes both. It works the same when p2 is on another core and its samples arrive through a ring. If an input stops, p3 drops its oldest kept sample and logs a warning once.
  - Phases become state. Falcon 1's RippleDetector had a burn-in loop before its detection loop; here it is `if (burningIn) { update statistics; return; }`.
  - A processor that acts on time checks the time itself on every pass, counting either CPU time or samples. Anything that ends an output, such as turning a TTL off 1 ms after a trigger, is timed in CPU time, so it ends even if the input stops. Such a processor must run on a spinning core (see **Waiting for data**).
- **A processor is a struct with an `Options` struct**: a `process()` function, a constructor and a destructor, plus an `Options` aggregate that holds only the processor's options, so they are set by name with designated initializers. The constructor acquires resources (memory allocations, sockets, file descriptors) and the destructor releases them. Internal state, such as filter history, lives in the processor, outside `Options`. No virtual functions: they would force one `process()` signature on every processor. Later, C++26 reflection can read `Options` and `process()`'s parameters (its inputs and outputs) for the UI (other options for later: **Processor type alternatives** in beyond_prototype.md).
  - Options are always named, never positional (non-negotiable): `Filter ap {300, 6000}` would also compile with highpass and lowpass swapped.
  - **Every option is a compile-time constant**, so the compiler can fold options into the instructions and use them for array sizes and loop counts. No option changes while the engine runs: changing one means generating, compiling and spawning a new engine program (see **Boundaries**), so every processor starts again from scratch (e.g. a detector's burn-in statistics are lost).
  - So `Options` is a template parameter, after the stream type: `Filter<RippleRaw, {.highpass = 300, .lowpass = 6000}> ap;` (compiler support: see **The bundled clang supports C++26** under **Boundaries**). It can't be nested inside its processor, because it must be declared before the processor's template, so it gets its own name, e.g. `FilterOptions`. The constructor takes no arguments and reads the options from the template parameter.
  - **Exceptions only outside the loop**: a constructor that fails (e.g. a socket that won't open) throws. The engine catches the exception and logs it, and the graph doesn't start. Nothing in the loop throws. Exceptions cost nothing while none is thrown.
    - C++ then destroys the processors already built on that core, in reverse order, and never builds the later ones.
    - The failing processor's own destructor doesn't run, because C++ runs a destructor only for a fully built object. So a resource acquired before the throw is either released before throwing or held by a member with its own destructor (e.g. a socket wrapper), which C++ does run. This matters most for device state the OS doesn't reset when the program exits, e.g. a device left acquiring.
  - Rejected: optional `on_init()` and `on_destroy()` hooks, which `graph.cpp` would call through helpers that check with a concept (`HasOnInit`, `HasOnDestroy`) and `if constexpr` whether the processor defines them. A constructor and destructor are standard C++, and C++ runs them automatically.
  - Rejected: no constructor, with the processor itself an aggregate and each resource a member built from options declared above it (`int port = 6363; Socket sock {port};`). A member declared above an option it reads gets an uninitialised value, possibly without a compiler warning; checks across options (e.g. highpass below lowpass) have no natural place; and an aggregate can't have private members, so `graph.cpp` could set internal state by name.
- **The channel count is a compile-time constant**, so the compiler knows every loop's length and every buffer's size (e.g. 32 float32 channels are exactly two AVX512 vectors, or four AVX2 vectors). A processor never hardcodes it: the same code runs at 32, 64 or 2000 channels, and different streams can have different counts (e.g. the branches of a channel split, shape 9 in graph_shapes.md). A new channel count means regenerating and recompiling `graph.cpp`.
- **Stream type**: the compile-time facts about a stream (channel count, sample rate, sample type) are grouped in one named type, and each processor takes it as a template parameter:
  ```cpp
  using RippleRaw = Stream<32, 30000, float>;   // written once

  Filter<RippleRaw, {.highpass = 300}>         ap;
  SpikeDetector<RippleRaw, {.threshold = -50}> spikes;
  ```
  Adding a fact later changes only `Stream`, not any processor. A processor whose output stream differs from its input takes both stream types and derives what it needs from them: a decimator from 30 kHz to 1 kHz gets its factor of 30 from the two types and needs no option for it. The generator writes these types; for the prototype we write them by hand.
- **Options** (params) are primitives (int, floating point, bool, string), enums, fixed-size arrays of these (e.g. per-tetrode thresholds), or a path to a file, such as a filter coefficients file or an IMRO file. All of them work as template arguments as they are, negative and fractional values included (C++20), except strings (see below). A value of the wrong kind, such as 1.5 for an int option, is a compile error.
  - **String options use `Str`**, one string type written once in the engine: a processor declares `Str name;`, and `graph.cpp` writes `.name = "Abc"`. Neither `std::string` nor a pointer to a string literal can be a template argument. A string option's length is known when `graph.cpp` is compiled, so nothing needs a string that grows at run time: `Str` is a fixed-capacity character array of 256 bytes, so a string has at most 255 characters plus the terminating zero. A longer string is a compile error, never cut. Its bytes sit in the program's read-only data, not in processor state. Rejected for now: C++26 `std::define_static_string`, which would only remove the length limit and needs reflection in the compiler.
  - **A file option is only its path**, a string; the constructor reads the file at run time with ordinary file I/O. So the file's contents are not compile-time constants and never set an array size or a loop count. Rejected: copying the file into the program at compile time with C++26 `#embed`, more machinery for no gain outside the loop (see **How to prioritise**).
- Processors are stateful or stateless. Each is a concrete type in one translation unit, so the compiler can inline a whole core loop into one fused unit.
- **Processor authors are trusted.** A processor is hot-path code that works directly on memory for speed, so the interface doesn't try to make mistakes impossible. It prevents a mistake only where that costs nothing at run time and doesn't limit how a processor is written (e.g. `In` and `Out` below); everything else is left to processor tests (see **Processor tests** in beyond_prototype.md).
- **`process()` signature**: `process()` takes one struct, `Connections`, declared inside the processor, with one member per connection. Every member is an input `In<T>` or an output `Out<T>`, where `T` is the connection's kind (see **Buffers**): a sample buffer, an event buffer, or a state value such as `bool`. Example: `struct Connections { In<Samples<S>> filtered; Out<Events<Spike, 32>> spikes; };` and `void process(Connections connections);` (a sample buffer is a `Samples<S>`, an event buffer an `Events<E, N>`, see **Capacity** under **Events**). `process()` returns nothing, and its connections are the processor's fixed set of inputs and outputs; a variable number of inputs is not a realistic need. `graph.cpp` names every connection at the call, `spikes.process({.filtered = ap_buf, .spikes = spikes_buf});`, as in the sketch under **graph.cpp**, and `In` and `Out` bind to the buffers and values. A processor's internal state (e.g. filter history) is not in `Connections`: it lives in the processor.
  - Why a struct: two connections of the same type in the wrong order are a compile error instead of a wrong experiment. It costs nothing at run time: measured with four and with ten connections, once `process()` is inlined both forms compile to the same instructions. Out of line, the struct is copied to the stack on every call (a few ns, see **Inlining every `process()`** under **Open questions**). Rejected: one parameter per connection, which compiles with a swap; argument comments at each call (`/*filtered=*/ap_buf`) checked by clang-tidy's `bugprone-argument-comment`, which catch a swap only when linting.
  - **Required and optional connections**: a member that `graph.cpp` must always pass is a plain `In` or `Out`; it has no default, so C++ rejects a call that leaves it out. A member that `graph.cpp` may leave out is a `std::optional` of its `In` or `Out`, e.g. `std::optional<Out<Events<NoiseLevel, 1>>> noise = std::nullopt;`, and `process()` puts both the calculation and the publishing for it under `if (connections.noise) { connections.noise->push(...); }`. `graph.cpp` leaves it out when nothing is connected, a constant fixed when `graph.cpp` is compiled, so once `process()` is inlined, clang deletes the block, calculation included; out of line, it stays behind one run-time branch. Measured: with the connection left out, an inlined processor compiles to exactly the same instructions as a copy of it that never had the connection, both on its own and inside a running core's loop. When it is connected, `Out` still clears the buffer at the call.
    - A use without the check compiles: C++ has no nullable type that the compiler forces to be checked (`*` and `->` on an empty `std::optional` are undefined behaviour). It is caught by clang-tidy's `bugprone-unchecked-optional-access`, which analyzes a processor only where a pipeline or a processor test instantiates it (measured). It is also caught by libc++ hardening at run time (see **Hardening** in beyond_prototype.md), and by processor tests that run each processor with every optional connection left out (see **Processor tests** in beyond_prototype.md).
    - Rejected: every form that makes such a use a compile error, as too complicated to use or read: a wiring struct as a template argument, read with `if constexpr`; a loop, macro or lambda that names the buffer only inside the block (the macro's `break` and `continue` also act on its hidden loop); one function per optional connection (`process_noise()`). Also rejected: a handle whose `push` does nothing when not connected, since publishing on a missing buffer must never happen.
  - The engine rejects at compile time a member of `Connections` that is neither `In` nor `Out`, so an author can't get a direction wrong, and writing to an `In` is a compile error. Graph users never see these errors: the generator connects only matching inputs and outputs.
  - `process()` itself is never a function template: C++ doesn't deduce template arguments through the conversion to `In` or `Out`, so `template <typename E, int N> void process(In<Events<E, N>> in)` fails to compile when `graph.cpp` passes an `Events<Spike, 32>`. Every type comes from the processor's own template parameters instead (e.g. `EventLogger<Spike, 32>`, see **Events**). This costs nothing: the generator knows every connection's type and writes it out.
  - `In` and `Out` are thin references, so the compiled loop is the same as with plain references. Reflection can later read `Connections` for the UI: direction from the wrapper, kind from `T`, name from the member's name.
  - Rejected: `const&` for inputs and `&` for outputs. An author who forgets `const` turns an input into an output without an error, and every author would have to be told to remember it.

### graph.cpp
`graph.cpp` holds the processors with their options, their buffers, a ring wherever a connection crosses cores, and one loop per core.

**In/out buffers.** `process()` reads its input buffers, writes its output buffers and returns nothing, like an audio plugin. `graph.cpp` declares every buffer and passes the buffers to each processor's `Connections`, so the data flow is visible in the loop. Chosen for clarity: there is one way to hand over data.
- As fast as returning values: each buffer is a local variable of its core's function, touched only by the inlined loop (see **Processors**), so the compiler can keep the buffer's count in a register. The samples go through L1 either way. This holds only while buffers stay local to their core's function.
- A processor can take several input and output buffers.
- The engine owns the buffers, so the generator can later let processors share buffers, or let a filter overwrite its input in place, without touching any processor (see **Buffer reuse** in beyond_prototype.md). The UI's cache view can count every buffer from `graph.cpp` (task D.5 in beyond_prototype.md).
- **No copies across rings**: a ring can hand a producer its next free slot as the output buffer, so the producer writes straight into the ring, as the input does in the sketch. A ring reader points its output buffer at the slot it reads.
- **`Out` clears an output buffer**: when `graph.cpp` passes a buffer to an `Out` (see **`process()` signature** under **Processors**), `Out`'s constructor clears it (count 0). C++ builds the whole `Connections` argument before `process()` starts, so the buffer is cleared right before its producer runs, on every call. So a processor that returns at once leaves its output empty, and neither a processor author nor `graph.cpp` can forget to clear it. Clearing right before the producer, not at the start of the pass, keeps feedback connections working, since they read the previous pass's output. It costs one write per buffer per pass. A state value passed to an `Out` is never cleared.
  - Rejected: wrapping every output in `out()` at the call (`lfp.process(raw_buf, out(lfp_buf))`), which did the clearing and showed the outputs in the loop. With `Out`, the signature already shows the direction, and a state value would have needed `out()` too without being cleared.

The sketch is an example with more processors than the prototype's graph. Its ring function names are placeholders, `Buffer` stands for `Samples<S>` or `Events<E, N>` (see **Operations a processor uses** under **Buffers**), and stream types (see **Stream type** under Processors), the first template argument of every processor, are left out for brevity.

```cpp
Ring input_to_core2, lfp_to_ripple, ripple_to_trigger;   // inserted where a connection crosses cores

void core1() {
    NeuropixelsInput<{.port = 6363}> input;

    while (running) {
        Buffer& slot = input_to_core2.next_slot();  // the input writes straight into the ring
        input.process({.samples = slot});           // polls the hardware; Out clears slot first
        input_to_core2.publish(slot);               // makes the samples visible to core 2, if there are any
    }
}

void core2() {
    Filter<{.highpass = 300, .lowpass = 6000}> ap;
    SpikeDetector<{.threshold = -50}>          spikes;
    Decimator                                  lfp;        // factor 30 comes from its two stream types
    Trigger                                    trigger;
    PCIeParallelOutput                         ttl;
    Buffer                                     input_buf, ap_buf, spikes_buf, lfp_buf, ripple_buf;   // owned by the engine
    bool                                       ttl_high;   // a state: keeps its value until trigger changes it

    while (running) {
        input_to_core2.read(input_buf);                     // points input_buf at the next ring slot, or leaves it empty
        ap.process({.raw = input_buf, .filtered = ap_buf});            // Out clears ap_buf right before the call
        spikes.process({.filtered = ap_buf, .spikes = spikes_buf});    // every process() returns at once if its input is empty
        lfp.process({.raw = input_buf, .decimated = lfp_buf});         // has output only every 30th sample
        lfp_to_ripple.write(lfp_buf);                                  // copies into the ring
        ripple_to_trigger.read(ripple_buf);
        trigger.process({.spikes = spikes_buf, .ripples = ripple_buf, .ttl_high = ttl_high});  // ttl_high is a state, so not cleared
        ttl.process({.high = ttl_high});
    }
}

void core3() {
    RippleDetector<{.threshold = 3}> ripple;
    Buffer                           lfp_buf, ripple_buf;

    while (running) {
        lfp_to_ripple.read(lfp_buf);
        ripple.process({.lfp = lfp_buf, .ripples = ripple_buf});
        ripple_to_trigger.write(ripple_buf);
    }
}
```

### Per-core loop
- **One loop per core.** Each core runs one thread, pinned to that core, and each pass of its loop calls every processor on that core once.
- **Each core's processors are declared inside that core's function** (as in the sketch under **graph.cpp**), so processors of different cores never share a cache line (false sharing). Rings are then the only memory that cores share.
- **Start barrier**: each core builds its processors, then waits at one barrier shared by all cores (e.g. C++20 `std::latch`). Once every core has arrived, all loops start together; if a constructor failed on any core (see **Exceptions only outside the loop** under **Processors**), every core returns instead. Without the barrier, the input could start filling a ring while another core is still failing, and the full ring would log a second, misleading error.
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
- **A connection carries one of three kinds**, and nothing else flows between processors: samples (all channels of one time point; defined by sample type, channel count and rate), events (see **Events**), or a state value (see **State values** below).
  - Rejected: merging samples and events into one kind, a buffer of records of one type with a capacity per pass, where a sample is one more record type. It changes nothing for speed, latency, alignment or cache: both are a pointer plus a count with compile-time types, and alignment comes from the record type either way. Less engine code is not a goal.
- **A buffer is a pointer to data plus a count**: 0 or 1 sample with its header, or the events produced on this pass (zero or more). Its storage belongs to the engine, but a producer can point it at data that already exists elsewhere (a ring slot, a socket receive buffer), so nothing is copied to hand data over.
- **Each sample has a header** of three values, about 24 B, set by the input processor:
  - its sample number: 64-bit, counted at the input's rate, with the same meaning on every device. Processors use it to match samples from different inputs, count samples and stamp events. The input derives it from the device's clock and never counts samples itself, so a lost sample shows up as a gap instead of shifting every later number without an error;
  - the device's own timestamp for that sample, in the device's own unit, which links Falcon's samples to the device's recording (e.g. Trellis, for the correctness check under **Agreed**). Processors never calculate with it. Example: Neuralynx counts its clock in µs, so sample numbers 0, 1, 2 at 32 kHz have device timestamps 31.25 µs apart; on Ripple, the device timestamp counts samples, so the two values are equal;
  - its arrival time (TSC), for the latency check under **Agreed**. The prototype's chain takes a sample from the ring to the TTL in one pass, so the engine logs the latency as the TTL write time minus the arrival time of the sample read in that pass.
  - Falcon 1 recorded the same two times (arrival time and device timestamp). Flags wait until something needs them.
- **The header travels with its sample.** A processor reads its input's header as `in.header`, and starts each output sample with `out.set_header(header)`, which copies the given header and sets the count to 1, so no sample exists without one. The same call serves a processor that passes on a header and one that builds a new one (an input processor, a test-signal generator, a spike-count binner fed by events). `set_header` is also where the processor decides that it outputs a sample on this pass: a decimator from 30 kHz to 1 kHz calls it on 1 pass in 30, and without the call the output stays empty. Usually it passes its input's header; a processor that outputs a sample late (e.g. after a lookahead) passes the header it kept with that sample, and one with two sample inputs passes one of their headers, whose arrival time is then what the latency log measures from. A processor with a sample input never makes up a new header: a made-up sample number breaks matching across inputs and gap detection, and a new arrival time makes the latency log measure from the wrong moment.
  - Rejected names: `emit`, `new_sample`, `start`, `forward` with a separate `create`, and `out.header = in.header` (setting a field that also makes the sample present is a hidden side effect). For the header itself: `stamp`. Whether a buffer holds a copy of the header or points at it in a ring slot is the engine's choice.
  - Rejected: one header per chain, handed by `graph.cpp` to the processors that need it. After a processor that outputs late, it would give every later processor the wrong sample number without an error.
- A count of 0 means nothing new on this pass.
- After a burst, the extra samples wait where they already are (in the ring or the socket buffer) and are handed over one per pass.
- **State values**: small results that describe a state, such as "the TTL is high" or "a ripple is ongoing", are plain values passed by reference. They keep their value until their producer changes it, and stay in registers once inlined. Anything that means "new on this pass", such as a detection, goes in a buffer, so `Out` clears it (see **`Out` clears an output buffer** under **graph.cpp**).
- A buffer's contents are valid only until its producer's next call, which overwrites them: for the rest of the current pass, or into the next pass for a feedback connection (see **Order** under **Per-core loop**). Several processors can read the same buffer in that pass (e.g. `input_buf` is read by both `ap` and `lfp` in the sketch). A processor copies what it needs to keep and never keeps a pointer into a buffer.
- A buffer filled by a ring reader looks the same as one filled by a processor on the same core, so a processor can't tell whether its input crossed cores.
- **Operations a processor uses**, each on the wrapper that allows it, so an `In` can only be read and an `Out` only written (see **`process()` signature** under **Processors**):
  - `In<Samples<S>>`: `is_empty()`, `header`, and `load(v)`, which returns channel vector `v` as a float32 SIMD register of the compiled instruction set (`__m512` or `__m256`, see **One processor per stream** under **Processors**).
  - `Out<Samples<S>>`: `set_header(header)` (see **The header travels with its sample** above) and `store(v, x)`.
  - `In<Events<E, N>>`: `is_empty()`, `size()`, and a loop over its records (`for (const E& e : in)`).
  - `Out<Events<E, N>>`: `push(record)`, or `push(header, record)` for a stream event, which also sets the record's header (see **Events**), e.g. `spikes.push(in.header, Spike{.channel = c})`.
  - A state value's `In` and `Out` read and assign like the value itself (`if (stim_on)`, `ttl_high = true`).
  - `S::vectors`: the number of channel vectors, i.e. the padded channel count divided by 16 on AVX512 or by 8 on AVX2.
  - No single-channel access: a processor that needs only a few channels gets them from a packer upstream, which copies them into a smaller stream (see **Channel selection** in beyond_prototype.md).
  - Mistakes left to processor tests (see **Processor authors are trusted** under **Processors**):
    - `store` without `set_header` compiles, and the processor seems to output nothing, since the count stays 0 (see **`Out` clears an output buffer** under **graph.cpp**), so the whole chain downstream stays silent. Only sample outputs have this case: an event is pushed whole in one call and a state value is assigned. Rejected: `set_header` returning the only object that has `store` (`auto sample = out.set_header(in.header); sample.store(v, x);`), one more line per sample processor and one more type for authors.
    - A processor that calls `set_header` but doesn't `store` every vector (e.g. an early `return` between them) leaves the skipped channels holding an older sample's values under the new header, and downstream reads them as real data. Example: artifact blanking stores zeros into the stimulated vectors but nothing into the others, assuming they stay "unchanged". A habit avoids it at no cost: each loop iteration computes one `result`, starting from the input where that makes sense, and ends with one `store`. Rejected guards: a test build where `set_header` fills the output with NaN; `set_header` always writing zeros first; counting `store` calls and checking them after `process()`; an all-at-once `set_header` that takes a function and calls it for every vector itself (`out.set_header(in.header, [&](int v) { return ...; })`), so the processor no longer writes its own loop.

### Events
A second, lightweight connection type for sparse outputs such as spikes, ripple onsets and decoder results. Within one core they are handed over in event buffers. Between cores they go through event rings (see **Event rings between cores** in beyond_prototype.md).
- **One record type per kind of event**: a plain fixed-size struct derived from the empty `struct Event {};`, either directly (e.g. a timer's `struct ScheduledEvent : Event { u32 count; };`) or through `StreamEvent` for an event from a stream (see below), e.g. `struct Spike : StreamEvent { u32 channel; };`. The record type is part of the connection's type, so the compiler rejects connecting ripples to a processor that expects spikes. Processors that accept any kind of event (TTL output, recorder, logger) are templates over the record type, constrained to `std::derived_from<Event>`; the generator fills in the record type and the capacity from the connection (e.g. `EventLogger<Spike, 32>`, see **Capacity** below).
  - Rejected: taking the base type, `In<Events<Event, N>>`. An `Events<Spike, N>` buffer is not an `Events<Event, N>` buffer: the records have different sizes, and copying a `Spike` into an `Event` keeps none of its fields. The processor needs the concrete type to read the fields (e.g. through reflection).
  - `Event` holds no sample number, because not every event comes from a stream: a timer such as EventGenerator has no sample to point to, and a made-up number in a field shared by all events would mean something different for each producer.
  - `Event` exists although empty, so that a field every event needs can be added later in one place instead of in every record type, including those in labs' extensions.
  - Rejected: one record type for all events (e.g. sample, channel, value), whose fields would mean different things for different producers, so a consumer could misread them without an error.
  - No virtual functions: they would add a hidden 8 B pointer to every record, and a record holding a pointer can't be copied byte for byte into a ring, shared memory or a file.
- **An event from a stream carries the whole header** of the sample it came from (see **Buffers**): the sample number for matching (the correctness check under **Agreed**, a spike-count binner), the arrival time for the latency log once events cross cores, and the device timestamp to find the event in the vendor's recording. It costs 24 B per event, and events are rare (most passes have none). Rejected: only the sample number (8 B), which would leave the latency log without an arrival time after a ring hop.
  - The header sits in a shared base, `struct StreamEvent : Event { Header header; };`, so it is defined once. A processor that needs the header (e.g. a spike-count binner, which builds its output's header from its spikes' headers) requires `std::derived_from<StreamEvent>`, so connecting timer events to it is a compile error; a processor that takes any event (logger, TTL output) requires `std::derived_from<Event>`.
  - The producer passes its input's header to `push` (see **Operations a processor uses** under **Buffers**): designated initializers can't name a base's members, so it can't go in the record's braces.
  - Rejected: a `header` field in each record type, which every author would repeat and which processors could find only by its name; one header per event buffer, which is wrong when one pass's events come from different samples (e.g. a processor that outputs a spike a few samples late, after finding its peak, or one that merges two spike streams).
- **No serialization code**: whatever saves or shows records (recorder, logger, UI) reads their fields through C++26 reflection, so an author only declares the struct (compiler support: see **The bundled clang supports C++26** under **Boundaries**).
- **The prototype's SpikeDetector** emits a `Spike` on the sample where a channel crosses the threshold, without waiting for the peak. Richer spike records come from separate processors later (see **Spike records** in beyond_prototype.md).
- **Capacity**: an event buffer has room for every channel firing in the same sample: a stimulation artifact crosses threshold on all channels at once, and a buffer too small for that would lose events. Each producer fixes at compile time how many events it can emit in one pass, e.g. one per channel for a spike detector. The same will hold for event rings.
  - The capacity is part of the buffer's type, `Events<E, N>`: the producer's output says it (e.g. `Out<Events<Spike, 32>>`), and `graph.cpp` declares the buffer with the same type. A consumer takes `N` as a template parameter too (e.g. `EventLogger<Spike, 32>`), filled in by the generator, since `process()` can't deduce it (see **`process()` signature** under **Processors**). `push` doesn't check the capacity: the producer never pushes more than `N` in one pass.
  - Rejected: the capacity outside the type, with `Events<E>` pointing at storage that `graph.cpp` sizes from a `max_events` constant each producer declares. It would keep `N` out of consumers' types, at the cost of a second declaration per buffer in `graph.cpp`.

### Rings
- A ring is inserted only where a connection crosses from one core to another: one ring per connection, in either direction. Two cores can have several rings between them. Each ring counts toward the cache of both the writer's and the reader's core.
- **Size**: an adjustable number of samples, always a power of 2.
- **One writer, one reader (SPSC)**, the simplest version: the prototype's only ring has one writer and one reader, and SPSC shares the fewest cache lines between the two cores. The old TODO list planned an SPSC design that minimizes cache misses. One writer with many readers is in beyond_prototype.md (**Rings with many readers**).
- **The header** (see **Buffers**) **sits in the slot**, in front of the samples: the simplest layout. Since samples fill whole cache lines, the header adds one cache line per sample (3 instead of 2 at 32 channels). Headers in a separate array would save half a line per sample, a later optimisation (see **Ring header layout** in beyond_prototype.md).
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
- **One run per engine program.** Start means the UI spawns the engine program. The program builds every processor (see **Start barrier** under **Per-core loop**): if a constructor failed, or a core's thread couldn't be started, it logs the error and exits; otherwise it runs the loops. Stop is one signal for the whole graph, never per processor: the loops end, the destructors release the processors' resources, and the program exits. So every run builds its processors from scratch.
  - The stop signals are SIGINT (Ctrl+C) and SIGTERM. Every thread blocks them from the start, and the main thread takes them with `sigtimedwait` and clears `running`. Otherwise the kernel could deliver a signal to any thread, including a realtime core's, and interrupt its loop.
- A config change needs a new engine program anyway (see **Every option is a compile-time constant** under **Processors**).
- An engine program that stays alive between runs is in beyond_prototype.md (**Engine states**).

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
- At start, the input processor checks that the device really runs at the compiled rate. If it doesn't, its constructor throws and the graph doesn't start (see **Exceptions only outside the loop** under **Processors**).
- Lower rates inside a graph come from processors that output only every N samples (see **Processors**).

## Open questions
The processor interface as actual C++ types, since it is what's expensive to change (see **How to prioritise** under **Agreed**). All settled: the sample's header, the buffer types `Samples<S>` and `Events<E, N>` and the operations a processor uses on them (see **Buffers**), the event bases `Event` and `StreamEvent` (see **Events**), and the processor's form with its `Options`, constructor, destructor and `process()` signature (see **Processors**). One question remains on it: how to name the members of `Connections` (see **`process()` signature** under **Processors**). `.clang-tidy` rejects names shorter than 3 characters, so the `in` in this spec's examples is out. The name is also the connection's name in the UI, so it should say what flows through it, e.g. `incoming_events`. The same goes for template parameters (`Events<E, N>`, `Samples<S>`): processor authors read and write them too, and no `.clang-tidy` rule checks them yet.
**Inlining every `process()`.** The disassembly of the monotonous event pipeline shows clang doesn't inline `EventLogger::process`, because `std::println` makes it too big. So that core's loop is not one fused unit (see **Processors**): it calls the logger on every pass, the call saves and restores six registers even when the buffer is empty, and since the buffer's address goes into the call, its count lives on the stack instead of in a register (see **graph.cpp**). A small processor is inlined: with a test processor between the generator and the logger that adds a constant to each event, the buffer between the generator and that processor disappears from the machine code entirely, with no stack slot and no register. Harmless on that non-realtime core, but any realtime processor too big to inline does the same. Open: should `graph.cpp` force every `process()` call inline (clang's `[[clang::always_inline]]` on the call statement, which processors never see), or leave it to clang for the prototype and check the disassembly?

Low priority, in beyond_prototype.md: under **Scaling to +2000 channels**, replace the note that several samples per call would help with the conclusion we reached instead: when every device delivers one sample every 33 µs, batching gains nothing, and splitting the channels across cores (shape 9) keeps each core's state in cache without adding latency.

What is still to verify is listed under **Agreed** (Ripple's documentation, Trellis recording alongside Falcon, the TSC across cores) and under **Boundaries** (the C++26 features in the bundled clang).


