# SfN brochure: Falcon

Working notes for the brochure handed out at SfN. Not engine documentation: engine facts come from [falcon1_differences.md](../../src/core/falcon1_differences.md) and [spec.md](../../src/core/spec.md).

## Decided
- A brochure, not a poster: [brochure.svg](brochure.svg), one A4 portrait page.
- The name is "Falcon", never "Falcon 2.0". The brochure is a promise of what the new engine will offer, so planned features may be listed.
- Tone: a science meeting, not marketing. Plain sentences a scientist would write, in experiment terms. No slogans, no compiler errors, ring buffers or thread pinning.
- Subtitle: "Flexible and user-friendly real-time closed-loop software for high-density neural signals". An earlier, longer subtitle was rejected as too long. The small line under it reads "Open source, actively developed, and installed with a single command.", and the footer status reads "Actively developed". The single-command installation is taken from the FENS poster.
- Never phrase anything as unfinished: no "in development", no "on the way". Both were rejected as negative, because they tell the reader the engine is not done. State positive facts instead.
- The intro is four lines long. Its first sentence is the Background sentence of the FENS poster, word for word, and must not be altered: "Closed-loop experiments causally infer the physiological role of transient neural events (such as replay or ripples) by decoding brain states from real-time streaming data and triggering external feedback with minimal latency."
- Message to get across after that sentence: Falcon is very fast and is the way to process dense data in real time. The intro states it as "Its new engine is designed to do this for hundreds of channels at 30 kHz on a standard CPU". FPGAs are not mentioned.
- What the user assembles from processors is called a "pipeline" in the brochure, never a "processing graph" or "graph".
- The website is falcon-neuro.org. It sits under the QR code, which is `Images/qr-code.svg` from the FENS poster folder.
- The FENS 2026 poster added only the Dart UI, and its latency plots measure the old engine, so the brochure shows no performance number.
- Open Ephys is left out entirely. Falcon no longer exchanges data with it and is meant to replace it as a whole; the brochure says neither.
- There is no separate "Built-in processors" section. The processors appear as plain text inside the three stages of the section "Real-time path from sample to stimulus", which exists to stress Falcon's real-time capability and small delays. It has no time arrow and prints no delay figure.
- The processors named, exactly these:
  - **Acquisition**: Neuralynx, Ripple Neuromed, Neuropixels. Blackrock and TDT are shown as upcoming.
  - **Processing**: IIR filter, FIR filter, Spike detector, Ripple detector, Burst detector.
  - **Output and recording**: TTL output, Save stream, Save events, ZeroMQ stream, WebSocket stream.
- The engine section is titled "Upcoming improvements" and holds five points:
  1. **SIMD across channels**: each processing step runs on 16 channels at once with AVX-512.
  2. **No cross-core penalties**: processors on the same CPU core exchange data directly, without queues or waiting.
  3. **Sample-by-sample processing**: samples are processed one at a time as they arrive, not in packets.
  4. **Just-in-time compilation of the pipeline**: the pipeline is assembled by drag and drop in the pipeline designer; before each run it is translated to C++ and compiled.
  5. **Live viewer rendered on the GPU**: a lightweight viewer in which GPU compute shaders reduce 30 000 samples per second to screen resolution, with no load on the CPU. The text does not mention trace colours.
- In the closed-loop figure the brain sits above the Falcon box and is the brain illustration from the FENS poster.

## About brochure.svg
- Look taken from the FENS poster: Ubuntu font, mauve `#6c535d` headings and rules, pink processor boxes, green for the new engine.
- The two program windows (pipeline designer, live viewer) are drawn mockups, not captures of a running program. The viewer's traces are generated, not recorded data: one second of 30 kHz signal per channel, drawn as it looks when many samples share one pixel column (a dense band between the minimum and maximum, with spikes standing out), because that reduction is what the viewer showcases. The baseline of every trace is flat: noise and spikes only, no slow waves.
- The closed-loop figure's example pipeline is Input → IIR filter → Ripple detector → TTL output, using only listed processors.
- The QR code, the brain and the three institution logos are embedded as images, each a single long `data:` line; everything else is plain SVG text and shapes that can be edited directly or in Inkscape.
- [engine.svg](engine.svg) is the earlier standalone closed-loop figure in blue and orange; the brochure does not use it.

## Not confirmed
- The QR code was not decoded to check that it points to falcon-neuro.org.
- Contact emails and the 2017 paper stand in for the poster's full author list and affiliations.

## Open questions
1. Should the closed-loop figure show a decoder again, as an example of a user-written processor? It was removed because no decoder is among the built-in processors.
2. Format: stay with one A4 side, add a back side, or make it a tri-fold?
3. Should the authors and affiliations appear?
