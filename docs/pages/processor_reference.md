---
description: "Reference of Falcon's built-in processors. Each has a page with its inputs, outputs and options and the computation it performs."
---

# Processor reference

Falcon's built-in processors. Each page gives the processor's inputs, outputs and options, the computation it performs and guidance on choosing its option values.

| Processor | Description |
|-----------|-------------|
| [`EventGenerator`](processors/event_generator.md) | Emits an event at a fixed rate, from a timer. |
| [`EventLogger`](processors/event_logger.md) | Writes the events it receives to the log, each one or their number in each second. |
| [`FirFilter`](processors/fir_filter.md) | Filters every channel with the same FIR filter, given as its taps. |
| [`IirFilter`](processors/iir_filter.md) | Filters every channel with the same recursive filter, given as a cascade of second-order sections. |
| [`LatencyBenchmark`](processors/latency_benchmark.md) | Measures, for each event, the time from the arrival of the event's sample to the benchmark. |
| [`LevelCrossingDetector`](processors/level_crossing_detector.md) | Emits an event on every sample where at least one channel crosses a level in a chosen direction. |
| [`NeuralynxInput`](processors/neuralynx_input.md) | Reads the raw signal of a Neuralynx acquisition system from the network and converts it to µV. |
| [`PCIeParallelOutput`](processors/pcie_parallel_output.md) | Turns each event into a pulse on one pin of a PCIe parallel port card. |
| [`Recorder`](processors/recorder.md) | Writes the samples and events of its connections into one file. |
| [`RippleDetector`](processors/ripple_detector.md) | Emits an event when the power of a ripple-band signal rises above a threshold that follows the signal. |
| [`SignalGenerator`](processors/signal_generator.md) | Generates a test signal on every channel, in place of acquisition hardware. |
| [`SpikeDetector`](processors/spike_detector.md) | Emits an event on every sample where at least one channel crosses a fixed threshold. |
| [`Trigger`](processors/trigger.md) | Emits a trigger event once every channel of a stream has been named by an event. |
