# Example Pipeline

This pipeline details the processing graph configuration for detecting hippocampal population bursts and decoding replay content in real time using the Falcon server framework.

## Pipeline Overview

The pipeline processes multi-channel streaming data to detect sharp-wave ripples (SWRs) and trigger closed-loop feedback mechanisms under 1 millisecond of latency.

[OpenEphys/Neuralynx] -> (NlxReader) -> (MultiChannelFilter) -> (RippleDetector) -> (DigitalOutput)

## System Requirements

Ensure the server instance allocates dedicated CPU resources to avoid context switching thread overhead:

* **Operating System**: Linux Enterprise / Ubuntu LTS
* **Processor Affinity**: Minimum 4 isolated CPU cores
* **Data Sources**: Open Ephys GUI or Neuralynx hardware stream

---

## Configuration Specification

Save the configuration below as `hippocampal_replay_pipeline.yaml`.

```yaml
graph:
  name: HippocampalReplayDetection
  sample_rate: 32000
  channels: 128

nodes:
  - id: reader_0
    type: NetworkReader
    port: 5555
    data_type: MultiChannel

  - id: filter_ripple
    type: BandpassFilter
    low_cutoff: 150
    high_cutoff: 250
    order: 4

  - id: detector_swr
    type: RippleDetector
    threshold_sd: 5.0
    min_duration_ms: 15

  - id: sink_ttl
    type: DigitalOutputSink
    hardware_line: TTL_0

connections:
  - from: reader_0:output
    to: filter_ripple:input
  - from: filter_ripple:output
    to: detector_swr:input
  - from: detector_swr:event
    to: sink_ttl:input
```

---

## Processing Nodes Reference

### NetworkReader
Initializes socket streams over TCP/IP protocol layers to receive raw binary matrices from recording amplifiers.

### BandpassFilter
Applies a zero-phase forward Butterworth filter across the assigned `MultiChannel` data types.

### RippleDetector
Tracks individual signal envelopes using root-mean-square transformations. It continuously updates the moving baseline variance metrics.

---

## Validation and Verification

Run the validation script to verify the graph loop optimization topology before production deployment:

```bash
falcon-cli --validate -f hippocampal_replay_pipeline.yaml
falcon-server --config hippocampal_replay_pipeline.yaml --daemon
```

### Expected Metric Layout

| Metric | Lower Bound | Target Range | Upper Bound |
| :--- | :--- | :--- | :--- |
| Latency | 0.2 ms | 0.4 ms | 0.8 ms |
| CPU Core Jitter | 1 μs | 3 μs | 10 μs |
| Memory Overhead | 45 MB | 64 MB | 128 MB |

## Troubleshooting Matrix

1. **Buffer Overflows**: Increase the thread priority or isolate the dedicated hardware cores using boot arguments.
2. **Dropped Network Packets**: Verify system network buffer allocations using kernel performance profiles.
