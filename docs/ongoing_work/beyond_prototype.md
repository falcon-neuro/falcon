# Beyond the prototype

Everything that is not part of the prototype engine: features, ideas and tasks for after the prototype works. The prototype spec is in [engine.md](engine.md). We leave all of this out of the engine spec discussion until the prototype engine works. Each item keeps the status it had before it moved here (decided, open or deferred).

**Order of work**, set by the user on 16 November 2026: once the engine runs its hand-written graphs on the Ripple system (see **Engine 0.0.1 before the UI** in the open questions of engine.md), the generator, then the UI's pipeline designer, then its ephys viewer; only after the UI, a bridge between the engine program and the UI, through which the UI reads streams, events and state values and writes state values (see **Runtime control**).

Order of work from here: first a usable product that can be downloaded from GitHub, with the basic detectors such as SpikeDetector, to ship by 29 November 2026 at the latest; from there we iterate. Replay-content detection, the ultimate goal (see **Decoding** under **Processors to port** in falcon1_differences.md), comes later. Said by the user on 18 November 2026: decoding comes right after the prototype, since it is what proves that SIMD solves Falcon 1's problems; a benchmark on a level crossing proves nothing, since a chain without a hop between threads takes under a microsecond. Its data types are parked with it (see **Streams and the decoder's data**). Still to list: what the first product needs of Falcon 1's other processors (see **Processors to port** in falcon1_differences.md), of the drag-and-drop pipeline designer (see pipeline_designer.md) and of the tests (see **Tests as whole runs** in [test_suite.md](test_suite.md)).

Moved here on Claude's judgment and not yet confirmed: **Heavy processors**, **Several chains on one core**, **Feedback connections**, **Buffer reuse**, **Event rings between cores**, **Slow readers**, **Experiment files**, **Channel labels and positions** and **Rings with many readers**. Whatever belongs in the prototype moves back to engine.md. **Event rings between cores** was parked by mistake and is in engine.md now (**Event rings** under **Rings**), but for what follows it under its heading here; the other candidates are under **Parked too early** in the open questions of engine.md.

## Engine

### Generator
Tracked in generator.md; what concerns the node editor is in pipeline_designer.md.

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

### Processor type alternatives
A processor is a struct with an `Options` template parameter (see **Processors** in engine.md). Option for later:
- A concept that checks at compile time what every processor must provide, so lab authors get a clear error. Since each processor's inputs differ, it can only check the parts they all share (e.g. that options are primitives or file paths).

### Rules that fail silently
A processor author must copy what they need to keep and never keep a pointer into a buffer (see **Buffers** in engine.md). Breaking this rule gives wrong triggers and no error, and keeping such a pointer compiles without a warning. When it breaks depends on where the buffer points: an engine-owned buffer is overwritten on the next pass, so the bug shows at once; a ring slot is overwritten only when the ring wraps around (e.g. 64 samples later), so a short test passes. The same processor can therefore work on one core layout and give wrong values on another.
- Mostly prevented already: processors read samples only through the buffer's load/store helpers (see **Sample type** in engine.md), so they never get a raw pointer to keep.
- Still possible: keeping a reference to a buffer and reading it in a later pass, when it already holds newer data or nothing. Candidate safety net: a debug build that fills every buffer with NaN after its pass, so such a mistake shows at once in a test. Release builds pay nothing. Not needed for the prototype, whose processors we write ourselves.

### FIR filter
FirFilter is in the prototype (see **FirFilter** in engine.md). Still to come:
- Falcon 1's FIR filter files (32 taps for Neuralynx's spike filter, 101 for a low-pass example): the generator writes their taps into `pipeline.cpp` like the biquad coefficients.
- Float32 filters: FirFilter computes in double for now. An FIR filter has no feedback, so its rounding is never amplified and can't make it unstable; float32 would halve its state (101 taps at 384 channels: 155 KB instead of 310 KB) and its vector operations.
- Decided: each filter kind is its own processor and its own UI node: `IirFilter` (the prototype's), `FirFilter` and `SlopeFilter` (see **Filter kinds** in [falcon1_differences.md](falcon1_differences.md)). The kinds differ in options (sections, taps, or a window size and two orders), in state and in delay, and a node of its own makes choosing a long-delay FIR visible in the graph.
- The names say IIR and FIR, the terms SciPy, MATLAB and papers use, because those name the trade-off a user picks between: a delay of a few samples, or a longer delay that is the same at every frequency. A coefficients file's `type` header names its processor (`iir`, `fir` or `slope`), and each node's file picker refuses a file of another kind.
- A slope filter (a Savitzky–Golay derivative) takes the window size, the polynomial order and the derivative order as its options and computes its taps from them at compile time, reusing the FIR code, so the graph keeps the values the user set rather than the taps.
- Coefficients as template arguments are encoded into the processor's symbol names. Hundreds are fine; thousands would bloat the symbols and slow the compile. If it ever matters, `pipeline.cpp` can declare them as a `constexpr` array and pass a reference to it as the template argument (allowed since C++20), so they stay compile-time constants. To be measured.

### Decimator
Decided: no decimator until a measured graph needs one. A decimator (e.g. 30 kHz to 1 kHz, shape 6a in graph_shapes.md) removes with a low-pass filter what the lower rate can't hold (anti-aliasing), then keeps one sample in N.
- Not needed for precision: IirFilter computes in double, so even very low cutoffs run at the full rate (see **Why double** under **IirFilter** in engine.md).
- It saves computation: LFP-band processing (ripples, theta) at 1 kHz does 1/30 of the work, which may matter at hundreds or thousands of channels. Slow readers (an LFP viewer or recording) need less data too, but they can reduce it themselves, off the realtime path.
- It costs latency: its output comes only every N samples (up to 1 ms late at 1 kHz), plus its anti-aliasing filter's delay. And an anti-aliasing filter that is too weak folds higher frequencies into the band without an error.

### IirFilter in float32 when proven safe
IirFilter computes in double (see **IirFilter** in engine.md), so an AVX512 register holds 8 channels instead of 16 and its state takes twice the bytes. If that ever matters (e.g. thousands of channels, see **Scaling to +2000 channels**), IirFilter could pick float32 at compile time for a filter whose coefficients float32 provably holds, and keep double for every other one, without asking the user.
- The check runs at compile time on the coefficients, which are compile-time constants, and picks the float32 or the double code with `if constexpr`. Float32 is kept only if every section passes both conditions:
  - Rounding leaves the same filter: round `a1` and `a2` to float32 and compute the section's two poles before and after (the roots of a quadratic; a `constexpr` square root by Newton's method suffices). Every pole must move by less than a tolerance, a small fraction of its distance to the unit circle, since that distance sets the section's bandwidth and cutoff.
  - The rounding noise stays small: each state update adds a float32 rounding error of about 6×10⁻⁸ of the state's size, and the section's feedback amplifies its power by (1 + a2) / ((1 − a2)((1 + a2)² − a1²)). The resulting noise, for the largest input the stream can carry, must stay below a bound.
  - The tolerance, the bound and the largest input are to choose. The stability check (see **IirFilter** in engine.md) then runs on the rounded coefficients, since those are what runs. The spike band at 30 kHz would pass; low cutoffs at the full rate would not (see **Why double** under **IirFilter** in engine.md).
- Cost: each instruction set needs a float32 and a double version, four in all, each tested against a double reference.

### Sample width: float16
The prototype uses float32 (see **Sample type** in engine.md). Processors load and store samples through the buffer's helpers, so switching to float16 later changes only the stream type, not any processor. Still open: whether to switch.
- The format must hold: spikes within about ±500 µV, LFP up to a few mV, and stimulation artifacts that drive the ADC to its full range (e.g. ±8 mV on Blackrock).
- **float32**: the helpers are plain loads and stores, with no conversions. 32 bits is more than the signal needs: acquisition ADCs deliver 10 to 24 bits, and the noise floor limits the useful resolution further.
- **float16** (IEEE half precision): half the bytes of float32. The step between neighbouring values is 0.25 µV or smaller within ±512 µV, 0.5 µV up to ±1,024 µV, and doubles with each further doubling of the value. The largest value is ±65,504 µV (±65.5 mV).
  - Zen 5 (the recommended CPU) converts between float16 and float32 but has no float16 arithmetic, and filters need more precision than float16 anyway (IirFilter computes in double, see **IirFilter** in engine.md). So the helpers convert every input to float32 and every output back: two conversions at every connection, including connections within one core, where the data is in L1 either way and the smaller size saves nothing.
  - On a ring between cores, float16 moves 2 cache lines per sample instead of 4 at 64 channels, and 63 instead of 125 at 2000 channels.
  - Unmeasured estimate: at 64 channels the conversions cost more time than the saved cache lines save. At 2000 channels the outcome is unknown and needs a benchmark.
  - Out of range: a float32 value beyond ±65,504 µV converts to infinity, not to the largest float16 value. An IIR filter that receives an infinity turns its state into infinity and then NaN, and outputs NaN until the graph is restarted. Neural channels can't reach that value, because their amplifiers clip first: Intan at ±5 mV, Blackrock at ±8 mV, NP1 at ±12 mV at its lowest gain (50). Signals that can reach it:
    - Auxiliary analog inputs (photodiode, accelerometer, analog sync lines) are in the volt range, and 1 V is 1,000,000 µV. Open: do they use the same stream format as neural channels?
    - Neuralynx's Digital Lynx is DC-coupled with a ±132 mV input range (to be checked), so a large electrode DC offset or a saturating artifact can exceed ±65.5 mV.
    - Derived values in µV² (e.g. signal power) exceed 65,504 for any signal above 256 µV (a 500 µV spike squared is 250,000 µV²).
- Does **64 channels first** (see **Principles** in engine.md) apply here too, i.e. choose the format that is fastest at 64 channels?
- The two priorities conflict here: float16 serves the embedded-systems approach (half the bytes in rings and caches), float32 serves scientific accuracy (no overflow to infinity, no rounding to float16 at every connection). Which one wins?

### Heavy processors
- **Heavy processors** (e.g. a decoder) get one or more cores of their own (see **Splittable processors**); that is how "big" processors are isolated. What moving a processor to another core buys depends on where it sits:
  - **On a serial path** (p1 → p2 → p3 → …), it doesn't lower latency, since one sample still passes every step in order, plus two core-to-core hops. It only gives each core its own time budget per sample (pipelining), so do it only when one core can't keep up.
  - **On a parallel branch** (e.g. ripple branch and spike branch both feeding a Trigger), both branches process the same sample at the same time. Latency at the join drops from `a + b` to `max(a, b + 2h)`, where h is one hop. The saving is at most the smaller branch (best when branches cost about the same), minus two hops.
  - It only helps when the branch is on the critical path (the longest path to an output) and costs much more than 2h. A decoder qualifies; a threshold doesn't.
  - Separate outputs on separate cores don't wait for each other's branches.
  - Splitting also gives each branch its own L1/L2. If the branches together don't fit in one core's L2, this removes cache misses and lowers latency even for cheap branches.
  - Hop cost: see **Rings** in engine.md.
  - Costs: one more busy core (heat), and rings take cache on both cores.

### Several chains on one core
- Chains on one core delay each other. A sample that arrives for one chain waits while another chain on the same core does real work. Example: core 1 runs the chains [p1 p2] and [p4 p5 p6] (see the example under **Per-core loop** in engine.md). If sample N−1 arrives from core 2 just after core 1's ring reader checked the ring, p4 gets it only after p1 and p2 have processed sample N+1 on the next pass. A processor that does heavy work only every N samples (e.g. a decoder) would delay a per-sample chain on the same core by its whole computation, which is why heavy processors get a core of their own (see **Heavy processors**).
- Since heavy processors have their own core, the chains that share a core are light ones. When two light chains have work in the same pass, the one later in the processor order waits for the other, at most for the duration of the other chain's work in that pass (e.g. a few hundred ns). The user's processor order (see **Order** under **Per-core loop** in engine.md) decides which chain waits. Yielding and hardware waits do not help here (see **Waiting for data** in engine.md).
- If the delay between chains matters, the user drags one of the chains onto another core in the node editor. The engine does nothing special. Moving a chain costs one more busy core: more heat, and cores are limited (a 9950X3D has 16, one is reserved for the OS, and in the first implementation each input takes one). In the example it adds no ring, since p3 → p4 already crosses cores; if p4 also read p2 directly, that connection would become a ring too.

### Feedback connections
Decided:
- Feedback is mostly a state (e.g. an adaptive threshold, "a stimulation is ongoing", the last stimulation's sample number), passed by reference like any state (see **Buffers** in engine.md). A state keeps its value, so its reader sees it whenever it runs.
- Across cores, a feedback connection is an ordinary ring, or for a state a cross-core state value (see **State values** under **Buffers** in engine.md).
- Within a chain, feedback through a buffer (events or samples) is a known weakness of the engine, not worth more engine work:
  - The reader sees the producer's output from the previous pass (see **`Out` clears an output buffer** under **pipeline.cpp** in engine.md), so it has seen the result for sample N before it processes N+1.
  - It sees that output during one pass only, and live that pass is usually an empty one. So the reader must read its feedback input even when its other inputs are empty, and copy what it needs (the rule for several inputs, see **Processors** in engine.md).
  - A reader that skips it misses feedback without an error, e.g. the last sample of every 12-sample Neuropixels 1.0 packet.

### Trigger rules
The prototype's Trigger fires once every channel has had an event, as Falcon 1's EventSync did (see **Trigger and outputs** in engine.md). Richer rules, each counted in samples so replays reproduce them: at least K of the channels, so one quiet channel can't block it; only some channels; a number of spikes, or of inputs (e.g. spikes and ripples), within a window, as Falcon 1's EventFilter did with its detection criterion and sync time; a lockout after each trigger, so a burst can't trigger repeatedly; and blocking events that veto triggers for a while, as EventFilter's block duration did.

### Pulse processor
Planned once several output processors need pulses (see **Trigger and outputs** in engine.md): the pulse logic moves out of PCIeParallelOutput into a processor of its own between Trigger and the outputs, trigger events in and the line's state out, so each output only writes a state. Its state must reach the output on the same core (see **State values across cores**).
- Name: PulseGenerator.
- Growth path: one output mapping each kind of event to an action per line (high, low, toggle, pulse), as Falcon 1's DigitalOutput did, which also covers a line that stays high as long as something lasts (start and stop events).

### State values across cores
Moved into the prototype and decided: a reader on another core gets the latest value (see **Across cores a reader gets the latest value, without waiting** under **Buffers** in engine.md). What is still open is under **State values across cores** in the open questions of engine.md.
- Later optimisation, of doubtful gain: pack the cross-core state values that one core produces into one cache line, where the prototype gives each a line of its own. It saves lines in the reading cores' caches, and moves one line instead of several when several values change in the same pass. But a reader then fetches the line for changes to values it doesn't read, and one value that changes often makes every reader of the line fetch it on every pass.
- Ruled out: one line holding values that different cores produce. Each write would take the line from every other writer and reader.

### Runtime control
Open: Falcon 1 let clients such as the UI read and write a processor's states while the graph ran (its `external` state permissions), e.g. to watch a value or to switch stimulation on and off during a session. Here every option is a compile-time constant and changing one restarts the engine program (see **Every option is a compile-time constant** in engine.md). Decided: it is needed, and it goes through state values: whatever the UI changes during a run is a state input of the processor that reads it (see **Parameters changed during a run** in the open questions of engine.md). Open: how the UI's write reaches the realtime cores.

### Buffer reuse
Decided:
- **Buffer reuse**: the processors in one core's loop share buffers instead of each owning a separate one, to keep the cache footprint small. Example: a loop of 5 processors does not need 5 separate buffers. Since the engine owns the output buffers (see **pipeline.cpp** in engine.md), this needs no change to any processor.

Open (formerly open question 9): how buffers are assigned, i.e. which connections may share a buffer.
- Fan-out: `neuropixels_input_to_ap_filter_and_lfp_decimator_sample_buffer` is read by both `ap_filter` and `lfp_decimator` (see the sketch under **pipeline.cpp** in engine.md), so no processor may overwrite that buffer until both have read it in the current pass.
- Feedback connections hold the previous pass's output (see **Feedback connections**), so that buffer must survive into the next pass and cannot be shared with other connections.
- A buffer can point into a ring slot or a socket receive buffer (see **Buffers** in engine.md). A processor that writes its output into such a buffer in place would corrupt data that other readers of the same ring still need.
- Who decides which connections share a buffer: the generator of `pipeline.cpp`, which sees the whole graph and the call order on each core, or each processor (e.g. a filter that declares it can work in place)?

### Event rings between cores
The prototype's event ring copies events in and out (see **Event rings** under **Rings** in engine.md). For later:
- No completeness tracking: a processor that needs complete events (e.g. spike binning) runs on the same core as the processor producing them. The event ring's writer cache line can reserve space for a "processed up to sample N" field, in case it's ever needed.
- Reading without a copy: `EventBuffer<EventType, Capacity>` holds its events inside itself, so it would have to point at the ring slot instead, as a sample buffer can (see **Buffers** in engine.md).
- Writing an event in place: the event buffer hands the producer an event to fill and the producer commits it, so `push` copies nothing, e.g. a channel mask of 1.25 KB at 10,000 channels (see **Detection events are channel masks** in engine.md). It adds an API and two silent mistakes: a forgotten commit drops the event, and an empty mask is pushed with no channel.

### Richer spike events
SpikeDetector's events say only which channels crossed on which sample (see **Detection events are channel masks** in engine.md). Richer events, each made by its own processor (or an optional output of SpikeDetector, to decide when it is built) from the detector's crossings and the filtered stream, so the path from detector to TTL never waits for them:
- the peak amplitude, about 0.1–0.3 ms after the crossing;
- per-group marks for clusterless decoding, e.g. `template <int ChannelsPerGroup> struct GroupSpike : Event { u32 group; float peak[ChannelsPerGroup]; };` (32 B for a tetrode);
- the waveform, e.g. 40 samples per channel of a tetrode (656 B), about 20 samples late (shape 14 in graph_shapes.md).

In all of them, `sample` is the sample number of the threshold crossing, so every event about one spike carries the same number and they can be matched. Open: whether such an event then carries the crossing sample's header, with the peak's position as a field, or the peak sample's header.
- **Peaks**: a SpikePeakDetector fed by SpikeDetector's crossings and the filtered stream can't disagree with SpikeDetector's threshold, and shows its late output as a node of its own; an optional peak output on SpikeDetector keeps one node, whose outputs then come on two timings. Either works; neither changes the masks.
- **When SpikeDetector fires**: an option choosing the moment, e.g. the threshold crossing (the earliest), the peak, or the crossing back. The peak gives a spike time that depends less on amplitude (a larger spike crosses the threshold earlier) and comes with the amplitude, but is known only once the signal turns, a sample or more later, so its event comes late and carries the peak sample's header. The crossing back gives how long the spike stayed beyond the threshold. Every choice is still one mask per sample, so the event type doesn't change.

### Slow readers
- **Slow readers** (viewer, recorder, audio) subscribe to what the hot loop publishes and never slow down the hot cores. The viewer is just another processor in the graph.
- A slow reader that falls behind: decided, for every reader alike, the recorder included (see **The same for a ring to a slow reader** under **Rings** in engine.md).
- **The end of a recording**: every loop stops at once when the run ends, so what a slow reader hadn't yet read from its rings never reaches its file: the last few ms of a run that lasts hours. Parked as too little to matter. A way to close it, inside `next_pass()` alone: realtime cores end their loops at once, so no pulse follows a stop, and a core that isn't realtime keeps running passes until every realtime core's loop has ended and one of its own passes then brought it nothing through a ring. It doesn't cover a core that isn't realtime feeding another such core through a ring, or a realtime core that never ends its loop.

### Recording formats
Recordings need proper file formats, such as NWB. Decided: later, since supporting NWB isn't affordable now. Until then Recorder writes a format of its own, one file for all its connections, as a session in NWB is one file (see **Recorder** in engine.md). Not designed yet: which formats, and how Recorder writes them.

### Experiment files
- **Each experiment is saved as one plain zip file** (e.g. `my_experiment.zip`) containing `pipeline.cpp` and every resource it uses.
  - Each recording stores a copy of the experiment zip it was recorded with, so every dataset shows exactly which graph, filters and IMRO produced it (recording itself is discussed later).
  - Resources such as filter and IMRO files can live anywhere the user likes.
  - When the user picks a resource, Falcon copies it into the experiment zip, so later edits, moves or deletes of the original don't affect the experiment.

### Paths on Debian
A plan for the user experience of the whole Falcon app on Debian, including where its files go: each run's directory (under its pipeline's, see **Each run writes into a directory of its own** in engine.md), experiments, settings and the installed program, e.g. following the XDG base directories.
- An idea: one directory per pipeline, `~/falcon/<pipeline name>/`, holding its `pipeline.cpp` and assets, with its runs in `runs/`. Taken up for the prototype on 21 November 2026: see **A pipeline's directory** in the open questions of generator.md. A run's directory is named by its date and time, or by a name the user gives it (e.g. `without_calcium_blockers`); if that name is taken, Falcon offers to add a number or the date and time.

### Privileges
The engine program runs as the user instead of with `sudo`, holding only the privileges it needs as Linux file capabilities on the program (`setcap`), e.g. `CAP_SYS_RAWIO` for the PCIe card's I/O ports and `CAP_SYS_NICE` for SCHED_FIFO. Then `~` is the user's home and a run's files are the user's (see **Each run writes into a directory of its own** in engine.md). Catch: a file capability belongs to the built file, so every newly compiled engine program must get it again, which itself needs root (e.g. once per build through `pkexec`).

### Group masks
An event about groups of channels, e.g. which tetrodes spiked on a sample. Until a processor decides per group itself, a consumer derives it from the channel mask (see **Detection events are channel masks** in engine.md) with an AND per group. Once one does, e.g. a detector that blocks a whole tetrode after it spikes, a mask with one bit per group becomes a second mask type.

### Tetrodes
Parked by the user on 18 November 2026: calculations on tetrodes come after the prototype, whose SpikeDetector reports channels (see **Detection events are channel masks** in engine.md). Not designed.
- Where a tetrode's four channels count as one thing, read by Claude from the two papers and from general practice, for the user to correct:
  - a spike belongs to its tetrode, not to a wire: one neuron's spike shows on up to four wires, so counting channels counts it up to four times. The 2017 paper's burst detection, with no decoder, ran a filter and a SpikeDetector per tetrode on 17 tetrodes and added their spike counts into the MUA;
  - after a spike the whole tetrode is blocked for a moment, as Falcon 1's SpikeDetector did while it searched the peaks;
  - a spike's amplitude on each of the four wires, for telling neurons apart, during the run (the decoder, or stimulation on one neuron's spikes) or afterwards (sorting);
  - a broken wire is left out of its tetrode.
  Where it plays no part: ripple detection and other work on the LFP, which takes chosen channels as plain channels.
- **Which of Falcon 1's tetrode work can't be SIMD**, asked by the user on 18 November 2026, who wants all of it in SIMD, without a processor per tetrode and with "a limited number of SIMD workers" (a worker is a core that shares the decoder's work on each spike, confirmed by the user). Claude's answer, not discussed, from the papers and not from the decoding processors' source (not at hand, see **Decoding** under **Processors to port** in falcon1_differences.md): none of the heavy work; what changes is what a lane holds.
  - **Every sample, a lane is a wire**, and one processor takes all tetrodes (see **One processor per stream** under **Processors** in engine.md): the filter, the threshold, whether a tetrode spiked (an OR over its wires), the block of the whole tetrode, each wire's peak during the block, and the spike count for the MUA. It needs every tetrode at a fixed place in the stream; a smaller group or a broken wire is a lane that a constant mask leaves out. Open: the layout, either being the packer's to make (see **Channel selection**). Side by side (t1w1 t1w2 t1w3 t1w4 t2w1 ...), 16 lanes hold 4 whole tetrodes and an operation across a tetrode's wires works inside one vector, on its mask. By wire (t1w1 t2w1 ..., then t1w2 t2w2 ...), it works between four vectors, and the tetrodes are padded to a multiple of 16 instead of the channels.
  - **Every spike, a lane is a Gaussian or a position of the grid**: the likelihood, and the product over tetrodes for each bin. A lane can't be a tetrode there: tetrodes spike at different times, and each has a model of its own size. Only here do several workers pay: a bin's spikes shared among a few cores, as many as the decoder is given and not one per tetrode (see **Splittable processors**).
  - **Not SIMD, and too small to matter**: writing a spike's event from the vectors, once per spike; what is one value per bin (the statistics on the MUA, the classifier's rule, the lockouts); the position, 25 a second; receiving datagrams and setting the digital output.
  - **What SIMD doesn't speed up**: an IIR filter along time, since a sample needs the one before and a cascade's sections run one after another, so its lanes are channels only; `exp` in the likelihood, which has no instruction of its own and needs an approximation whose error must be checked; and a model that doesn't fit in the cache, which waits for memory.
  Parked topics this touches: **Group masks**, **Richer spike events**, **Channel selection**, **Splittable processors** and **Streams and the decoder's data**.

### Channel labels and positions
- Stream metadata the prototype doesn't need yet: channel labels and positions. To be designed with cache in mind. Open: whether they are compile-time constants like the channel count and the rate (see **Rates** in engine.md), or runtime data that reaches processors in one of these ways:
  - a. passed once at graph start, read-only during streaming
  - b. stored next to the ring, so readers get it when they attach
  - c. sent in the stream, ahead of its first sample

### Streams and the decoder's data
Parked by the user on 18 November 2026 until the prototype works. A mistake in either stays local, to the packer and Recorder or to the decoder, so neither has to be right before it.
- **Which acquisition channel a stream's channel is**, raised by the user on 18 November 2026, not designed. A stream's channels are numbered from 0. So once channels are picked, by the packer (see **Channel selection**) or by an input that enables only some, channel 0 of the stream can be channel 24 of the acquisition system, and nothing records that. A recorded spike's mask, and a recorded sample, then name a channel that the analysis can't trace to its electrode. Said by the user: the packer needs engine work, and as of now none of this changes the pipeline designer.
  - Proposed by the user, not designed: a registry for the whole pipeline, in which each stream has a unique id or name and the list of its acquisition channels, e.g. `{"stream1": [25, 26, 27]}` for a stream of 3 channels that were channels 25, 26 and 27. The packer packs the values and sees to its output stream's entry, but the mapping is not private to it: anything can read it, e.g. Recorder for its file's description. Events keep their mask in the stream's own numbering.
  - A stream has a name today, e.g. `RippleRaw` (see **Stream type** under **Processors** in engine.md), but only as another word for its facts: two streams with the same channel count, rate and sample type are one type, which the compiler can't tell apart. A unique id would make each stream its own type.
  - What Falcon 1 did, read on 18 November 2026 from the fklab extensions' `datatypes/`: a time series carried one text label per channel, "0", "1" and so on unless a processor gave its own, agreed between two processors before the run, looked up by text during it, and written with the data. A spike named no channel: it belonged to one tetrode's group of at most 16 channels and held a timestamp and an amplitude for each of them (see **Spike amplitudes** in falcon1_differences.md).
  - Decided by the user on 18 November 2026: the registry is fixed at compile time, as everything is whenever possible, so nothing fills it during a run.
  - Open, to raise one at a time: whether it is part of the stream's type or beside it (processor code sees the stream's type, so that is expensive to change later, see **How to prioritise** in engine.md); and which input a channel number refers to when a pipeline has several inputs.
- **Streams and the data a decoder passes**, raised by the user on 18 November 2026: the prototype stays basic, but its data types must be designed for what comes. What a decoding graph passes between processors, read from Sodkomkham et al. 2016 (Knowledge-Based Systems 94, kernel density compression) and from **Decoding** in falcon1_differences.md, and confirmed by the user as what Falcon 1 could do:
  - a tetrode's samples: 4 values in µV, every sample;
  - a spike with its amplitudes: a time and one amplitude per channel of the tetrode, per spike (see **Richer spike events**);
  - the animal's position: x and y in cm, also a head angle and a speed, 25 a second, on the camera's clock;
  - the encoding model: Gaussians, each a weight, a centre and a width, thousands per tetrode, built from spikes and positions and read at every spike; built before the decoding and loaded in the 2018 paper, updated after every bin in the 2016 one;
  - a tetrode's likelihood, and the decoded probability: one value per position of a grid, hundreds of values, once per bin (10 ms in the 2018 paper, 250 ms in the 2016 one);
  - the MUA: one value in Hz per bin;
  - the decision: an event, e.g. one that names the maze arm.
  What follows for streams, Claude's reading, not discussed:
  - A stream's channels are not always electrodes: x and y, or the positions of a grid. So what a channel is needs more than an acquisition channel's number (see **Which acquisition channel a stream's channel is**), and a stream needs its unit (µV, cm, a probability).
  - A stream's rate is not always the acquisition's, nor is its clock: bins counted from the samples, a camera with a clock of its own.
  - Channels come in groups, e.g. a tetrode's four, and the decoder works on one group at a time.
  - An event may carry one value per channel of its stream, e.g. a spike's amplitudes. Its size is fixed, so it is an event type like any other, and its channels are its stream's.
  - The model is none of the three kinds of connection: a large table that one processor builds and another reads. The user wants it designed first of these (see **Order of work** at the top). Its first question, open: whether a model is fixed during a run, so built from an earlier run and compiled into the pipeline as the 2018 paper's pause allowed, or changes while the graph runs, as the 2016 paper's does and as Falcon 1's Encoder did.
    - Weighed by Claude on 18 November 2026, not decided. A changing model is the harder one: its size isn't known when the pipeline is built; adding a spike searches every Gaussian for the nearest, heavy work beside the decoding; a writer on another core changes what the reader holds in its cache; and the position must be matched to the spikes during the run. It is also worth more to an experiment: no pause to build the model, a model that follows drift, and decoding in a place the animal is still learning. The proof that SIMD is faster needs only the fixed one, which is also what the 2018 paper's numbers were measured on. A way between, proposed: the design allows a changing model, with a largest size fixed at compile time and a count that may grow, and the fixed model, built first, is one that nothing writes during the run.
    - Said by the user on 18 November 2026: none of it touches SpikeDetector or RippleDetector, so even a major mistake in the model's design stays inside the decoder. Claude agrees: only the decoder's processors would use the model, a spike with amplitudes is a new event type beside the mask, and a new fact about a stream changes `StreamInfo` and no processor (see **Stream type** under **Processors** in engine.md). The registry of streams is local too (said by the user, correcting Claude): in the hot loop only the packer needs it, Recorder reads it once at start, and no detector or filter reads it at all. What outlives a mistake in it is only what Recorder wrote of it into files.
  - Parked topics this touches: **Channel labels and positions**, **Group masks**, **Richer spike events** and **Heavy processors**.

### Channel selection
- What happens when the user only wants some channels, e.g. channels 16 to 24, and discards the rest? It touches the stream type (channel count), the padding of channels to a multiple of 16, and where the other channels are dropped (in the input, or in a processor).
- Decided direction: a **packer** processor (working name; also called input formatter or rearranger) copies the needed channels into a smaller stream, padded to a multiple of 16, so the processors downstream see only those channels and stay plain vector loops (processors have no single-channel access, see **Operations a processor uses** under **Buffers** in engine.md). Its option is the list of channels to keep, and its output stream type has the packed channel count.
  - It can run on the input's core, right after the input, so the ring to the processing cores carries only the packed channels: e.g. 16 of 384 float32 channels take 2 cache lines per sample instead of 25 (see **Hop cost** in engine.md).
  - Still to design: how the packer picks channels out of the input's vectors.

### Rings with many readers
The prototype's rings have one writer and one reader (SPSC, see **Rings** in engine.md). With SPSC, fan-out to several cores writes the same sample into each ring, which a buffer that feeds rings does (see **A buffer that feeds rings** under **Rings** in engine.md). The alternative for later:
- One writer, many readers (SPMC): one ring per output, and each reader keeps its own read position. Fan-out needs no copies. Since a full ring is a failure, the writer never reads reader positions; each reader detects being overrun from the sample number. Slow readers subscribing to what the hot loop publishes fit this model.

### Ring header layout
The prototype puts each sample's header in its ring slot, in front of the samples (see **Rings** in engine.md). Later optimisation: headers in a separate array next to the slots. Samples fill whole cache lines, so a header in the slot always adds one cache line per sample, while a separate array (two headers per cache line) adds half a line. The saving shrinks as channel counts grow: 3 vs 2.5 lines per sample at 32 channels (17% less), 5 vs 4.5 at 64 (10%), 25 vs 24.5 at 384 (2%), 126 vs 125.5 at 2000 (0.4%). The cost is a little more bookkeeping: two arrays instead of one.

### Ring bundling
- Possible later optimisation: bundle all connections from one core to another into a single ring per direction (one write position to check, fewer shared cache lines). Harder when the bundled connections have different rates or mix events and continuous data.

### Hardware wait
- **hardware wait** (MWAITX on AMD, UMWAIT on Intel): the core sleeps until the ring's write-position cache line is written. No OS involved, caches kept, low power. Added only if a diagnostic on the target machine shows its wake-up time is close to spin.
- A hardware wait watches a single memory address range (one cache line) per core. A core whose loop reads several rings (e.g. one per chain) can sleep on only one of them; data arriving in another ring does not wake the core.
- A state value written by another core doesn't wake it either (see **Across cores a reader gets the latest value, without waiting** under **Buffers** in engine.md). Its processors copy the new value on its next pass, so when its next sample arrives: harmless for a processor that acts on samples, too late for one that must act on the state alone, e.g. stopping a stimulation that the UI switched off while the input is silent.
- A hardware wait doesn't make the option not chosen there, the reading core waiting for the producer's core, any cheaper: the reader still waits for the slower core, only without spinning.
- For warnings (see **Logging** in engine.md): every core's `warn()` also bumps one shared "warning posted" counter, and the main thread sleeps on its cache line instead of polling every 100 ms, so a warning shows within microseconds and the writing core makes no system call. Catches: the OS sees the waiting thread as running, so it holds an OS core while it waits; it still needs a timeout, for stop signals and the data rings' drop counts, and wakes at every timer interrupt anyway; and the code differs between AMD and Intel and must check that the CPU and kernel allow it from user space.

### Huge pages
- Each core's memory (rings, processor state, buffers) in 2 MB pages instead of 4 KB pages, so the CPU needs far fewer address translations (TLB entries). At any channel count; the gain is to be measured.
- Pages reserved at boot, not Linux's transparent huge pages: those are merged by a kernel thread (khugepaged) while the graph runs, which sends TLB-flush interrupts to the realtime cores.
- Engine-only, no processor changes: e.g. each core thread's stack lives in a huge page, so processors and buffers stay local variables of the core's function (see **Per-core loop** in engine.md).

### Vector chains
- Register reuse inside a chain: pass one SIMD vector of channels at a time through consecutive per-channel processors.
  - Values then stay in registers instead of going through L1 at every processor boundary (~2–5 ns per boundary at 384 channels; an estimate, to be measured).
  - Deferred as premature optimisation.
  - Adding it later without touching processors needs per-channel processors written in a per-vector form: a per-sample "do I run?" plus a per-vector `process()` that returns its output instead of writing it.

### Inlining every `process()`
In the prototype, the compiler decides which `process()` calls to inline (see **Processors** in engine.md). What follows was measured with clang, the compiler until 18 November 2026 (see **The compiler is GCC** under **Boundaries** in engine.md), and is not yet looked at in GCC's machine code. A call it doesn't inline runs on every pass, even when its input is empty, and since the buffer's address goes into the call, the buffer's count lives on the stack instead of in a register (see **pipeline.cpp** in engine.md).
- Measured on the monotonous event pipeline, with a test event logger whose `process()` printed with `std::println`: clang didn't inline it, because `std::println` made it too big, so that core's loop was not one fused unit and the call saved and restored six registers on every pass. A small processor was inlined: with a test processor between the generator and the logger that adds a constant to each event, the buffer between the generator and that processor disappeared from the machine code entirely, with no stack slot and no register.
- Harmless on that non-realtime core, but any realtime processor too big to inline does the same.
- Measured on a realtime core too: in the FIR chain latency pipeline (128 channels), clang called each of the 7 FirFilters' `process()` out of line, with its `Connections` struct on the stack, and inside it reloaded both buffers' sample pointers for every channel vector, since it couldn't rule out that its own stores change them.
- Open: should `pipeline.cpp` force every `process()` call inline, or keep leaving it to the compiler and check the disassembly? With clang it would have been `[[clang::always_inline]]` on the call statement, which processors never see; what GCC offers for it is to find.

### Arithmetic across processors
Once a core's `process()` calls are inlined into one loop (see **Inlining every `process()`**), the compiler optimises across processors: it removes buffers between them (measured with clang) and folds integer arithmetic (`3 × (2x + 5)` becomes `6x + 15`).
- It doesn't do that for float32 samples by default, since IEEE floating-point arithmetic isn't associative: `3 × (2x + 5)` and `6x + 15` round differently. Flags such as `-ffast-math` or `-fassociative-math` allow it, but then results differ from a reference script that computes in the written order (see **How we judge that the engine works** in test_suite.md) and from one build to the next. Rejected, even for processors that opt in: accuracy comes first (see **Priorities** in engine.md).
- Related: clang contracts `a * b + c` within one expression into a fused multiply-add by default (`-ffp-contract=on`), which rounds once instead of twice: slightly more accurate, but different from a reference that rounds twice. It applies to plain arithmetic, not to SIMD intrinsics, which processors mostly use (IirFilter uses fused multiply-adds on purpose). Kept, and since 18 November 2026 passed to the compiler as a flag (decided by the user): GCC's own default, `-ffp-contract=fast`, also fuses across expressions, and without the flag it did so, since the flag changed the instructions of nearly every pipeline. `-ffp-contract=off`, so that only an explicit fused multiply-add fuses, was not taken.
- Each processor loops over its channel vectors on its own, and the compiler doesn't fuse two such loops by default; keeping a vector in registers from one processor to the next is **Vector chains**.
- An exact alternative is fusion by the generator: it could merge consecutive linear processors (e.g. two gains, or a gain into an IirFilter's gain) into one before compiling, as long as the merged form gives the same results within the tolerance agreed for that processor.

### False sharing
- Done in the engine: what one core writes is kept 128 bytes apart from what another core touches (see **Per-core loop** in engine.md). What is still open is under **128 bytes between cores** in the open questions of engine.md.

### Exact replay
Not a goal: a replay should give about the same triggers as the live run (99% matching is enough), and for the same input most processors give exactly the same output anyway (see **Principles** in engine.md). If exact replays are ever needed, e.g. for regression tests, these break exactness:
- logic that depends on CPU time, e.g. a TTL pulse timed in CPU time;
- joins whose result depends on timing, e.g. taking the newest value from another core instead of matching samples by sample number;
- the AVX512 and AVX2 versions of a processor rounding differently, so a value right at a threshold can cross it on one machine and not on another.

### Jitter and joins across rates
- **Jitter from uneven passes** (the pass in which an every-N processor fires takes longer) and **join semantics across rates**: low priority. Joins are the processor's own business.

### Log ring
The engine's own ring for `warn()` and `log()` is in engine.md (**In the loop** under **Logging**). The libraries considered instead:
- The realistic library is Quill: one queue per thread, a polling backend thread, TSC timestamps, a bounded queue that drops and counts, ready file and console output. Against it: its default queue grows by allocating, so it must be configured bounded and preallocated per thread; its macros expand at every call site, which may keep the compiler from inlining `process()` (see **Inlining every `process()`**; to measure); it formats through a formatter per type, not C++26 reflection; and it is a large dependency with a thread of its own, whose queues and drop counts are separate from the engine's rings.
- spdlog's asynchronous mode doesn't fit: it wakes its thread through a condition variable, a system call.

### A cap per kind of message
Every kind of message gets at most one record per 100 ms in its core's ring (see **The throttle** under **Logging** in engine.md). Wanted later, not a priority: each kind sets its own cap, passed with its message.

### Runtime performance measurement
- Not the priority now. Measure, while the graph runs, the time between consecutive processors, including ring hops between cores, using the CPU's timestamp counter (TSC). This would show the user when a chain waits for another chain's work on the same core. Details for later:
  - Cost: each TSC read takes tens of CPU cycles (to be measured), paid at every processor boundary on every pass. Proposed: only in a separate measurement build of `pipeline.cpp`, never in a session's build, since reads between processors keep the CPU from overlapping one processor's work with the next, so the measured loop runs differently. A run-time switch would leave a branch at every boundary and still change the loop.
  - In that build the generator surrounds every `process()` call with TSC reads, and no processor changes. For an operator, the time around its call is its cost, and the time after it minus its input's arrival time is the latency so far. An input processor's call includes polling the device, so what counts there is the arrival time to the ring's publish. An output processor's cost is mostly its hardware write, once pulse timing has its own processor (see **Pulse processor**).
  - The UI shows each processor's share as a heat map: the border of its node turns red for the processors that take the most time. A toggle in the UI switches it between the mean and the 99th percentile (latency suffers from the spikes). Open: red relative to the other processors (one is always the reddest, even when all are cheap) or to the time a sample allows (33 µs at 30 kHz, so red means real time is at risk).
  - Realtime cores never print, so the timestamps would go through a ring to a non-realtime core (see **Logging** in engine.md): the engine runs a collector thread there that reads them and feeds the UI, which also runs on a non-realtime core (see the advised cores under **Machine setup** in engine.md).
  - TSC values from different cores are only comparable if the TSC is invariant and synchronised across cores; to be checked on the target CPUs.
  - Related: the arrival time in each sample's header (see **Buffers** in engine.md) and task A.2 (benchmark each processor on the target machine before the graph runs).

### Latency tests for the paper
Tracked in test_suite.md (**Latency tests for the paper** under **Latency measurement**).

### Scaling to +2000 channels
- Ignored while designing for 64 channels. Notes for the future:
  - A float32 sample is 8 KB (125 cache lines), so every core crossing gets about 5× more expensive.
  - Ring depth competes with cache: 1 ms of ring is 240 KB.
  - Processor state no longer fits L1: one 4-section filter is 128 KB (its state is double, see **IirFilter** in engine.md), more than Zen 5's 48 KB.
  - At one sample per pass (see **One sample per pass** in engine.md), such a filter reloads its state from L2 for every sample (estimate: a few hundred ns per filter).
  - Decided: no batching, more cores instead. Several samples per call would read each group of channels' state once per batch instead of once per sample, but a device delivers one sample every 33 µs, so collecting 8 samples means waiting about 267 µs for the last one, far more than it saves. Instead, the channels are split across cores (shape 9 in graph_shapes.md), e.g. 4 cores × 500 channels, so each core's filter state (32 KB) fits in L1. The cores run in parallel, so latency doesn't grow; the cost is more busy cores and the ring hops between them.
  - The sample width (float16 vs float32, see **Sample width: float16**) halves or doubles the sample and ring sizes; filter state stays double either way.

### Engine states
- Deferred. The prototype runs once per engine program: stop ends the program (see **Start, stop and config** in engine.md). Later, the program can stay alive between runs, with states such as ready and running, so a stop ends only the loops and a new start doesn't spawn a new program.

## UI
- **Ring fill levels**: the UI shows each ring's peak fill while the graph runs, so the user sees that the pipeline is healthy before a ring overflows (the prototype only warns once samples are dropped, see **Rings** in engine.md). 1 sample is healthy, more than 1 is warning color, above 75 percent is red warning (change graph, or give more buffer size). Cheap to track: the ring's writer already reads both positions for every sample, so it can keep the highest fill level since the last report, read by a non-realtime thread like the drop count. Related: task A.3.
- **Switch to realtime kernel**: a setting of the UI that installs the realtime kernel for the user (see **Decided** in release.md).
- **Log view**: the UI collects a run's log lines and shows them with colours, filtering and search. Headless, the console styles them itself (see **A line** under **Logging** in engine.md).
- **Live viewer**: the viewer is a processor in the engine and the UI is a separate program, so the viewer's data has to cross from the engine process to the UI process. Not designed yet, and not part of the prototype.
- The UI shows a cache view with per-core L1/L2/L3 usage, so the user sees when the graph is about to spill out of cache (see **Principles** in engine.md, and **Cache view** in pipeline_designer.md).
- **Viewing data without a graph** (UI topic, formerly an open question): maybe the UI runs a small generated engine, e.g. NeuropixelsInput → Viewer.
- **Buffers the cache view can't see** (formerly an open question). Processors keep their own buffers (e.g. p3's copies while it waits for another input). The cache view (see pipeline_designer.md) can only count them if their size is known before the graph runs.
- **Filter design**: the UI designs filters by calling SciPy. The user picks the family, the band, the cutoffs and the order; SciPy computes the coefficients, and the UI writes the coefficients file with its header (`type`, `sampling_rate`, see **IirFilter** in engine.md) and plots the filter's magnitude, phase and group delay at the stream's rate, so the user sees the cutoffs and the delay in ms before running. Until then, coefficients files are designed with SciPy outside Falcon.
  - Why SciPy: filter design is a chain of numerical steps (analog prototype, frequency prewarping, bilinear transform, grouping poles and zeros into sections) with edge cases such as very low cutoffs and high orders. Reimplementing it in C++ would need every family tested against SciPy anyway, since a design error gives a wrong filter without an error message.
  - So Falcon needs Python with SciPy on the machine, installed with Falcon.
  - Open, outside the hot loop: how the UI calls SciPy, e.g. running a bundled Python script as a child process, or embedding Python in the UI.
- **Extensions that users download** (the user's thought of 21 November 2026): ordinary users may get extensions that others wrote, perhaps through a downloader in the UI. Such a user doesn't build the UI (see **The UI is never built on a user's machine** in release.md), so the list of processors can then no longer be baked into the UI's program alone: the list needs an intermediate representation, a form between the processors' source and the UI: each extension brings its part of the list as a file, which its developer makes with the list script and the UI reads when it starts. The change touches the script's output and the UI's loading, no processor.
- **Options validated while they are typed**, required by the user on 18 November 2026 and parked by the user on 21 November 2026: the prototype has a Compile button that builds the whole pipeline and shows the compiler's errors instead (see **A Compile button builds the whole pipeline into its executable** under **Decided** in generator.md). The wish: the moment a value breaks a rule, its error shows in red at the option, e.g. "the threshold can't be 0". The compiler can't give that, since a check by it takes a fifth of a second at best and starts a compiler at each change. So, decided by the user on 18 November 2026: the rules become functions that the running UI calls (sketched below), and the compiler, which calls the same functions, stays the last check, when a pipeline is built; **The compiler checks a processor's options, and nothing else does** (in generator.md) then holds for that last check alone. Where the UI shows a broken rule (decided by the user that day): every rule names the one option it is shown at, which its writer chooses, usually the one its message speaks of, so a rule is a condition, a message and an option; all broken rules show at once, each in red under its option; and a rule against the stream is skipped while the processor's stream isn't known, and shows once it is.
  - **The form of a rule**, proposed by Claude on 18 November 2026, not decided; it is what processor code sees. The rules are functions inside the options struct, each rule one line that reads as a `static_assert` does: `rules.require(threshold != 0, threshold, "the threshold's sign gives the direction, so it can't be 0");`, where the second argument is the option itself, so its name can't be mistyped. Rules on the options alone stand in `check(Rules& rules)`; rules against the stream stand in a second function, `check_against_stream(Rules& rules, const StreamFacts& stream)`, with the stream's channel count and rate as values, which is simply not called while no stream is known, so no rule can reach for a stream that isn't there. The processor keeps one `static_assert`, which calls both through a helper of the engine's and has the first broken rule's message as its own. The script refuses any other `static_assert` about `options`. Weighed and not proposed: the rules as a table of data, each with a function of its own, which reads worse; and the option named by a text, which can be mistyped.
    - Said by the user on 18 November 2026: the form misses edge cases. Those Claude finds in today's processors and in what is planned, each with a first thought, none designed:
      - **A rule with no option to show at**, e.g. one about the stream alone (a processor that needs at least two channels): a rule without an option, shown on the processor's block.
      - **A rule about what is connected**: Recorder needs one name for each connection, which depends on the graph and on neither the options alone nor the stream.
      - **A processor with two streams**, the one it takes and the one it starts, e.g. a decimator whose factor must fit both rates: the second function gets both.
      - **A list whose length is a template parameter** (`taps`, `sections`, `slot_names`): the UI can't make that options struct for a length known only while it runs.
        - Proposed by Claude on 21 November 2026 as the case to settle first, since it alone can overturn the form, where the others add to it. Two ways, not decided:
          - **A list's rules run in the compiler's check alone**, and the form stays as proposed. `taps` and `sections` are never typed: they come from a file in one step (see **Importing from a file** in the open questions of generator.md), so their error would show when the pipeline is built, or at the import if the import starts the compiler's check. `slot_names` then needs no rule, if the designer itself keeps one name for each connection.
          - **A list's rules run in the UI too**: a list in an options struct becomes a type of the engine's, of fixed length in a pipeline and of any length in the UI, and a rule is written against what both offer. Its cost is in what processor code sees: a new list type in three options, while `process()` must still get an array whose length the compiler knows.
      - **A value that can't be read yet**, half typed or empty: no rule can run on it, so the UI needs an error of its own for it, and must say which rules it skips meanwhile.
      - **A message that holds a value**, e.g. which section is unstable, or the limit a frequency must stay under: a rule's message is a fixed text in the form proposed.
      - **An option that must be set**, whose default breaks a rule on purpose (a filter's taps, its design rate): a processor is red the moment it is added.
      - **What a rule can't give the UI**: the range a value may have, before anything is typed, e.g. for a control that stops at 2 and at 9. Turning the typed texts into values needs no reflection: the script knows each option's name and type and can write that code into the list.
- **Whether reflection can do the check**, asked by the user on 18 November 2026. Claude's answer, not decided: not as the check stands. Reflection works while a program is compiled, and sees declarations, not a `static_assert` or its condition; the values to check are typed while the UI runs. It could serve another design, the one not chosen under **The compiler checks a processor's options** in generator.md: each processor's rules written as an ordinary function, which the compiler calls on the options and the running UI calls on what the user typed, with reflection turning the typed texts into an options value. That would answer at once, with no compiler started. Its costs: a set form for every rule, which the script would have to require; the UI holding real values, where it holds texts today, among them lists whose length is a template parameter; the stream's facts handed to the rules as values, where they read them from its type; and the UI compiled with every processor's options and rules, where it has only their list.
  - How it would work, sketched by Claude on 18 November 2026 at the user's request, from a count in the source; nothing of it tried. Today's processors hold about 25 rules on options in 10 processors: most on one value (a threshold that isn't 0, a pin from 2 to 9), some against the stream's rate (a frequency below half of it, a filter designed for it), some between two options, three on a list (at least one tap, every tap a number, every section stable), and one on a text (a file's name).
    - Each options struct gets one function that returns the message of its first broken rule, or none, given the options and the stream's channel count and rate as values. The processor's single `static_assert` calls it, with the returned text as its message, which C++26 allows, so the compiler and the UI judge by the same code. The script refuses any other `static_assert` about `options`, so no rule goes unseen.
    - The UI is built with every processor's options and that function, and holds for each processor in the graph a real options value beside the texts. Reflection fills the value from the texts field by field: a number, a bool, a value of an enum by its name, a text. The stream's facts the designer has already (see **Streams** under **Done** in pipeline_designer.md).
    - The hard part is the three lists (`taps`, `sections`, `slot_names`), whose length is a template parameter: C++ can't make `FirFilterOptions<n>` for a length known only while the UI runs. Either their rules stay with the compiler, which the pipeline's build before a run asks anyway, so that about 22 of the 25 rules answer at once; or those structs and their rules are written over a list of any length.
    - Its size, roughly: the 25 rules rewritten as functions (mechanical); a file of reflection that turns texts into an options value, of the size of `event_fields.cpp` and unlinted like it; the UI's build taking in the processors' options, where an extension's need its vendor's headers; and the script's new refusal. More than the compiler's check, which needs the precompiled header, a call of the compiler and its message read.

## Hardening
Ways to catch bugs that the current build setup (`CMakeLists.txt`, `.clang-tidy`) misses. Deferred: no time during the prototype.
- **The C++ library's run-time checks came on with GCC** (18 November 2026, see **The compiler is GCC** under **Boundaries** in engine.md), not by a decision. `CMakeLists.txt` defines `_GLIBCXX_ASSERTIONS`, which GCC's library obeys and clang's ignored, so until then no build checked anything. Every build now checks, in the loop too, an index into a `std::array`, `std::vector` or `std::span` and the use of an empty `std::optional`, among others (tried on an array and on an optional: each ends the program with the library's message). So an optional connection used without its check is caught at run time (see **Required and optional connections** in engine.md). Open:
  - Whether the realtime build keeps them, since every check costs time in the loop: a compare and a branch never taken, which the compiler removes where it can prove the index valid. The latency measured with GCC includes them (see **The FIR chain's latency** in test_suite.md). To measure before deciding: one pipeline built both ways, compared by its assembly and by LatencyBenchmark.
  - What a failed check does. It ends the program on the spot, and the library's handler can't throw, so the check can't become an exception: no destructor runs and no output is set safe, unlike a failure of Falcon's own (see **An error is an exception** in engine.md).
- **Settings that look like protection but do nothing** (cheap to fix):
  - `CMakeLists.txt` also defines `LIBCXX_ENABLE_HARDENING`, which no library knows.
  - `.clang-tidy` sets options for `misc-assert-side-effect`, but `misc-*` is not in its `Checks`, so the check never runs.
- **More clang-tidy check families:**
  - `clang-analyzer-*` (the Clang Static Analyzer) follows code paths and finds null dereferences, use-after-free, leaks and reads of uninitialised values. It finds the most real bugs of all of these, but is slow.
  - `cppcoreguidelines-*`: uninitialised members, missing copy/move members (rule of five), object slicing, raw `new`/`delete`. Its pointer-arithmetic and `reinterpret_cast` checks flag SIMD intrinsic code on purpose, so they need a rule for where exceptions are allowed.
  - `misc-*`: const-correctness (`const` wherever possible) and assert side effects.
  - `concurrency-*`: C library functions that aren't thread-safe, which matters with one thread per core.
- **Sanitizer builds**: see the open questions of test_suite.md.
## Documentation
Tracked in website.md (**Documentation from the code**), with every other task on the website.

## Tasks
From the former TODO.md. Its other tasks are now part of engine.md.

- **A:**  Refactor Falcon engine
    1. Detect CPU microarch on the fly and change grub settings to isolate all cores but one for Falcon.
    2. Advanced, later: benchmark each processor on the target machine with synthetic data (at the configured channel count), so the UI can show whether each core keeps up before the graph runs. Needs careful design, since other programs running on the machine (e.g. a browser) skew the results.
    3. Advanced, later: track how many slots each ring actually uses during a run, and log a hint when it's far below its size (e.g. only 2 of 64 used → shrink to 4), since oversized rings waste cache.
    4. Check the UDP receive buffer limit (`net.core.rmem_max`) at startup. Debian's default (212992) caps socket buffers at ~416 KiB, only a few ms of Neuropixels data, so any short receiver stall makes the kernel drop packets where Falcon can't see them. If it's below 32 MiB, Falcon asks the user, and on approval sets it immediately and persists it in `/etc/sysctl.d/` (needs root, e.g. via pkexec), so users never set it by hand.
    5. Turn off interrupt batching (coalescing) on the network card that receives acquisition data. Batching makes packets sent 33 µs apart reach Falcon two or three at a time and adds a delay of its own (e.g. `ethtool -C <nic> adaptive-rx off rx-usecs 0 rx-frames 1`). Before Falcon turns it off itself, the UI checks the card and shows a visual warning when batching is on. That replaces the console message network inputs print when built (`src/core/misc.cpp`, see **Machine setup** in engine.md).
    6. Advanced, later: kernel bypass for network inputs (AF_XDP, or DPDK). The card writes packets straight into Falcon's memory with no kernel copy, and checking for a packet is a memory read, so an input can live in a processing core's loop instead of on its own core. AF_XDP can take only the UDP data and leave the TCP control connection to the kernel. Catches: an `electrodePacket` is 10,056 bytes, bigger than a 9,000-byte jumbo frame, so either Falcon reassembles the IP fragments or the sender sends smaller packets. Also, the card's receive ring becomes the buffer that has to prevent packet loss.
    7. Pin the acquisition network card's interrupts (`/proc/irq/<n>/smp_affinity_list`) off the processing cores: every queue's, since which queue gets the data depends on the sender's port (see **Machine setup** in engine.md). Open: OS core or the input's own core. While xipplib receives the packets, an OS core, where its thread runs.
    8. Turn off deep idle states (`/sys/devices/system/cpu/cpu<n>/cpuidle/state<k>/disable`) on Falcon cores that block; waking takes tens of µs.
- **C:** JIT: moved to **Tasks** in generator.md.
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
    4. to 7.: the node editor, its cache view, its bypass toggle and the order of processors, moved to pipeline_designer.md.
- **E:** Neuropixels
    1. Until imec provides Linux drivers, probes are run from a separate Windows PC with our own sender program. It uses imec's Windows API to control the probes and streams raw samples to Falcon over Ethernet. Falcon acts as the front end (like OpenEphys): the user manages probes from the Falcon UI, and the sender applies the settings.
    2. Two connections between Falcon and the sender: UDP for data (`electrodePacket`), plain TCP with length-prefixed JSON for control (probe list, IMRO = per-channel electrode site and gain table, start/stop, status).
    3. Samples arrive as raw ADC counts. Falcon converts them to µV and orders channels by depth using the config the sender reports as applied (not the one requested). Config changes are only allowed while streaming is stopped.
    4. IMRO editor in the Falcon UI. NP1 has 960 recording sites on the shank but only 384 channels; each channel N can read site N, N+384 or N+768 (bank 0/1/2), so a channel's number doesn't always match its height on the shank. The editor draws the shank, lets the user mark regions to record from, enforces one site per channel, sets gains, and saves/loads `.imro` files (SpikeGLX format) that are sent to the sender.
    5. Rewrite `src/processors/neuropixels_input.cpp` (the current version is a sketch). It runs alone on its own spinning core, checks the socket without sleeping (no `io_uring_wait_cqe`, no `notify_one()` per packet), and writes each packet's 12 samples into the ring to the processing cores. Its packet access should expose the samples as a structure of arrays, not only as the `electrodePacket` array of structures.

