# Glossary

The fundamental words of the engine docs, each with one meaning.

- **Channel**: one electrode's signal, the voltage one recording site measures over time. A probe with 32 recording sites gives 32 channels.
- **Channel value**: one channel's measurement at one time point: a single number, in microvolts.
- **Sample**: a snapshot of all channels at one time point: every channel's value, plus a header. Recording at 30 kHz gives 30,000 samples per second, one every 33.3 µs. A sample is never one channel's value alone.
- **Header**: a small label attached to each sample that identifies it: which sample it is (its number), when the device took it and when it reached Falcon, and how many samples were lost before it.
- **Stream**: the series of samples one source produces, one after another, e.g. everything one acquisition device sends. All samples of a stream have the same channels and come at the same rate.
- **Rate**: how many samples a stream has per second, e.g. 30 kHz. The time between two samples is the sample period.
- **Event**: something that happened at one moment, written down as a struct of its kind (its event type), e.g. "on sample 12,345, channels 17 and 40 crossed the spike threshold", or "the trigger fired". One event can hold several spikes of the same sample. Samples come at a fixed rate whether anything happens or not; events come only when something happens, so most samples bring none.
- **State value**: a single value that describes an ongoing condition and stays as it is until its producer changes it, e.g. whether a ripple is ongoing. Unlike samples and events, it doesn't mean "something new just arrived".
- **Processor**: one step of a graph that does one job, e.g. reading data from the acquisition hardware, filtering it, detecting spikes, deciding to stimulate, or sending the stimulation pulse.
- **Connection**: a link from one processor's output to another processor's input, drawn as a line in the graph editor. It carries one kind of data: samples, events or a state value.
- **Buffer**: the memory through which a connection hands its data from one processor to the next when both run on the same core: the first writes into it, the second reads from it. A sample buffer holds one sample or none; an event buffer holds the events produced on this pass, zero or more.
- **Ring**: the memory through which a connection hands its data from one core to another: a circular row of slots that the writing core fills in turn and the reading core reads in the same order.
- **Slot**: one sample's place in a ring.
- **Hop**: a sample's move from one core to another through a ring. It takes time, because the data must travel between the two cores' caches.
- **Graph**: the processors and connections that make up one experiment, as the user designs it in the editor.
- **Pipeline**: a graph written by hand in C++ as its own program, instead of designed in the editor.
- **Core**: one processing unit of the CPU; a CPU with 16 cores runs 16 things at once.
- **Loop**: what each core runs over and over for as long as the experiment runs: each of its processors in a fixed order, then from the start again.
- **Pass**: one turn of a core's loop, in which each processor on that core runs once.
- **Chain**: processors connected one after another on the same core, so a sample goes through all of them within one pass, without a hop.
- **SIMD** (single instruction, multiple data): one CPU instruction that does the same operation on several values at once, e.g. compares 16 numbers with a threshold in one step.
- **Channel vector**: a group of adjacent channels of one sample (16, or 8 on older CPUs) that the CPU processes with one SIMD instruction. **Lane**: one channel's place in a channel vector.
- **Mask**: a row of bits, one per channel, each saying yes or no for its channel, e.g. which channels crossed a threshold on this sample.
- **Hot path**: the work done for every single sample, from the input to the decision to stimulate. Every nanosecond spent there adds to the latency.
- **Realtime core**: a core reserved for the hot path; the operating system runs nothing else on it.
- **Trigger**: the decision to stimulate, and the processor that makes it.
- **Latency**: the delay between a sample reaching Falcon and what it causes, e.g. a trigger. Measured end to end, it also includes the time spent in the acquisition and output hardware.
