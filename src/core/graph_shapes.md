# Graph shapes

The atlas of graph shapes the engine must support. Every engine design decision (core placement, rings, joins) must handle all of them. The graph is not a DAG: feedback loops (shape 7) are supported.

The prototype runs shape 1 (see **Prototype plan** in [spec.md](spec.md)). The processor interface must not rule out any other shape.

How to read the drawings:
- A box is a processor.
- `--->` carries a continuous stream. A label on the arrow gives its rate.
- `...>` and `:` carry events: sparse records such as spikes.
- The drawings show the logical graph only. Any processor can be placed on any core; the engine inserts a ring wherever a connection crosses cores.

## 1. Chain
Processors in a line, each reading the one before it.

Example: Input → Filter → SpikeDetector → Output

```
+-------+            +--------+            +---------------+            +--------+
| Input |--30 kHz--->| Filter |--30 kHz--->| SpikeDetector |..events...>| Output |
+-------+            +--------+            +---------------+            +--------+
```

## 2. Fan-out
One output read by several processors.

Example: Input → {AP filter, LFP filter, Recorder, Viewer}

```
                    +-----------+
              +---->| AP filter |
              |     +-----------+
              |     +------------+
              +---->| LFP filter |
+-------+     |     +------------+
| Input |-----+
+-------+     |     +----------+
              +---->| Recorder |
              |     +----------+
              |     +--------+
              +---->| Viewer |
                    +--------+
```

All four read the same 30 kHz stream. The recorder and the viewer are non-realtime sinks (shape 8).

## 3. Diamond
Split, then rejoin, from the same source.

Example: Input → ripple branch + spike branch → Trigger

```
                +------------+       +----------------+
           +--->| LFP filter |------>| RippleDetector |....+
           |    +------------+       +----------------+    :
+-------+  |                                               :    +---------+
| Input |--+                                               +...>| Trigger |
+-------+  |                                               :    +---------+
           |    +------------+       +----------------+    :
           +--->| AP filter  |------>| SpikeDetector  |....+
                +------------+       +----------------+
```

Both branches process the same sample. Trigger combines their results, matching them by sample number.

## 4. Join at different rates
Two inputs that deliver results at different rates.

Example: Decoder (every 10 ms) + RippleDetector (every sample) → Trigger

```
+----------------+
| RippleDetector |-- every sample (30 kHz) ---+
+----------------+                            |    +---------+
                                              +--->| Trigger |
+---------+                                   |    +---------+
| Decoder |-- every 10 ms (100 Hz) -----------+
+---------+
```

Trigger gets a ripple result on every sample but a decoder result only every 10 ms, so it decides itself how to combine them (see **Processors** in spec.md). The decoder's input is spike counts per 10 ms bin (shape 6c).

## 5. Multiple sources, different clocks
Inputs from different devices, each on its own clock. Not a priority now.

Example: Camera position tracking + acquisition system → Decoder

```
+-------------------+
| Acquisition input |-- 30 kHz, acquisition clock --+
+-------------------+                               |    +---------+
                                                    +--->| Decoder |
+--------------+                                    |    +---------+
| Camera input |-- 30-60 Hz, camera clock ----------+
+--------------+
```

The two clocks drift apart, so sample numbers from the two sources don't line up.

## 6. Rate change
A processor whose output rate or connection type differs from its input.

Example: 30 kHz → 1 kHz LFP decimation; continuous → events (detector); events → continuous (spike counts per bin)

a. Decimation

```
+-------+            +-----------+           +----------------+
| Input |--30 kHz--->| Decimator |--1 kHz--->| RippleDetector |
+-------+            +-----------+           +----------------+
```

A decimator by 30 leaves its output empty on 29 of every 30 samples, so everything after it runs at 1 kHz.

b. Continuous to events

```
+--------+            +---------------+            +---------+
| Filter |--30 kHz--->| SpikeDetector |..events...>| Trigger |
+--------+            +---------------+            +---------+
```

c. Events to continuous

```
+---------------+            +--------+                       +---------+
| SpikeDetector |..events...>| Binner |--counts every 10 ms-->| Decoder |
+---------------+            +--------+                       +---------+
```

The binner outputs one spike count per channel per bin.

## 7. Feedback loop
A processor reads the output of a processor that comes after it. A feedback connection is one pass late (see **Feedback connections** in beyond_prototype.md).

Example: stimulation times fed back to blank the stim artifact at the input; adaptive threshold

a. Stimulation artifact blanking

```
+-------+     +---------+     +--------+     +---------+     +-------------+
| Input |---->| Blanker |---->| Filter |---->| Trigger |....>| Stimulation |
+-------+     +---------+     +--------+     +---------+     +-------------+
                   ^                                                :
                   :       stimulation times (one pass late)        :
                   +................................................+
```

The times at which stimulation happened go back to the start of the chain, where the blanker removes the stimulation artifact.

b. Adaptive threshold

```
+--------+     +---------------+     +-----+
| Filter |---->| SpikeDetector |....>| ... |
+--------+     +---------------+     +-----+
                  ^         :
        threshold |         : detections
                  |         v
               +------------------+
               | ThresholdAdapter |
               +------------------+
```

The detector's detections feed an adapter, which sets the detector's threshold.

## 8. Non-realtime sinks
Processors on non-realtime cores that read from the realtime path.

Example: Recorder, viewer, audio: must never slow the realtime path

```
+-------+     +--------+     +---------------+     +--------+
| Input |--+->| Filter |--+->| SpikeDetector |....>| Output |
+-------+  |  +--------+  |  +---------------+     +--------+
           |              |
===========|==============|=== realtime cores above, non-realtime cores below ===
           |              +------------+
           v              v            v
      +----------+    +--------+   +-------+
      | Recorder |    | Viewer |   | Audio |
      +----------+    +--------+   +-------+
```

The sinks read what the realtime path publishes and must never slow it down (see **Slow readers** in beyond_prototype.md).

## 9. Channel split/merge
One stream split into channel groups that are processed separately, then merged.

Example: One probe split by shank or region into separate branches, merged later

```
                                          +------------+
                         +--ch 0-127----->| CA1 branch |--+
                         |                +------------+  |
+-------+     +-------+  |                +------------+  |    +-------+
| Input |---->| Split |--+--ch 128-255--->| DG branch  |--+--->| Merge |---> ...
+-------+     +-------+  |                +------------+  |    +-------+
                         |                +------------+  |
                         +--ch 256-383--->| CA3 branch |--+
                                          +------------+
```

The regions and channel ranges are only an example (a 384-channel probe).

## 10. Bursty heavy compute
A processor that does a big computation every N ms and is idle in between.

Example: Decoder runs a big computation every N ms, idle in between

```
+--------+                       +---------+
| Binner |--counts every 10 ms-->| Decoder |---> ...
+--------+                       +---------+

  time:  0 ms         10 ms        20 ms        30 ms
         |            |            |            |
  work:  ######.......######.......######.......######....
         ^ big computation when a bin arrives, idle until the next bin
```

## 11. Multiple outputs
Several output devices, each driven by its own branch.

Example: Several stimulators or TTL lines driven by different branches

```
                +------------+     +----------------+     +------------+
           +--->| AP filter  |---->| SpikeDetector  |....>| TTL line 1 |
           |    +------------+     +----------------+     +------------+
+-------+  |
| Input |--+
+-------+  |
           |    +------------+     +----------------+     +------------+
           +--->| LFP filter |---->| RippleDetector |....>| Stimulator |
                +------------+     +----------------+     +------------+
```

## 12. Disconnected subgraphs
Graphs with no connection between them, running at the same time.

Example: Two independent experiments in one graph

```
Experiment A
+---------+     +--------+     +---------------+     +----------+
| Input A |---->| Filter |---->| SpikeDetector |....>| Output A |
+---------+     +--------+     +---------------+     +----------+

Experiment B (no connection to experiment A)
+---------+     +------------+     +----------------+     +----------+
| Input B |---->| LFP filter |---->| RippleDetector |....>| Output B |
+---------+     +------------+     +----------------+     +----------+
```

## 13. Multiple sources, same clock
Several inputs that share one clock but deliver at slightly different times.

Example: 2–4 Neuropixels probes → one decoder; their packets arrive at slightly different times

```
+---------+
| Probe 1 |-----+
+---------+     |
+---------+     |     +---------+
| Probe 2 |-----+---->| Decoder |
+---------+     |     +---------+
+---------+     |
| Probe 3 |-----+
+---------+

Packet arrival:

  probe 1:  [  n  ]          [ n+1 ]          [ n+2 ]
  probe 2:     [  n  ]          [ n+1 ]          [ n+2 ]
  probe 3:   [  n  ]          [ n+1 ]          [ n+2 ]
            ----------------------------------------------> time
```

The decoder matches samples from the probes by sample number.

## 14. Join after a delayed branch
A join where one branch delivers its result for a sample later than the other.

Example: A spike waveform extractor (needs ~20 samples after the peak) joined with a branch without that delay; samples are matched by sample number

```
                +---------------+     +-------------------+
           +--->| SpikeDetector |....>| WaveformExtractor |...+
           |    +---------------+     +-------------------+   :
+-------+  |                                                  :   +------+
| Input |--+                                                  +..>|      |
+-------+  |                                                      | Join |
           |                                                  +-->|      |
           |    +----------------+                            |   +------+
           +--->| RippleDetector |----------------------------+
                +----------------+
```

The extractor's result for sample N arrives about 20 samples after the ripple branch's result for sample N. The join holds the earlier result until the later one arrives, matching them by sample number.
