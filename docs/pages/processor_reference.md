---
description: "Reference of Falcon's built-in processors. Each has a page with its inputs, outputs and options and the computation it performs."
---

# Processor reference

Falcon's built-in processors. Each page gives the processor's inputs, outputs and options, the computation it performs and guidance on choosing its option values.

| Processor | Description |
|-----------|-------------|
| [IIR filter](processors/iir_filter.md) | Filters every channel with the same recursive filter, given as a cascade of second-order sections. |
| [Spike detector](processors/spike_detector.md) | Emits an event on every sample where at least one channel crosses a fixed threshold. |
