# TODO

- **A:**  Refactor Falcon engine
    1. Drop old lmax distruptor.
    2. Architect SPSC architecture that minimizes cache misses.
    3. Architect process scheduling engine for maximizing performance on all cores that dedicated to Falcon. Detect CPU microarch on the fly and change grub settings to isolate all cores but one for Falcon.
    4. Advanced, later: benchmark each processor on the target machine with synthetic data (at the configured channel count), so the UI can show whether each core keeps up before the graph runs. Needs careful design, since other programs running on the machine (e.g. a browser) skew the results.
    5. Advanced, later: track how many blocks each ring actually uses during a run, and log a hint when it's far below its size (e.g. only 2 of 64 used → shrink to 4), since oversized rings waste cache.
- **B:** Use SIMD
    1. Refactor existing scalar processors to SIMD using AVX512, with fallback to AVX2. Scalar implementation should be dropped and AVX2 should be mandatory to run Falcon.
- **C:** JIT
    1. Replace existing YAML based graph definition with "poor-mans-JIT".
    2. Ship with clang compiler and compile the graph from the generated .cpp file.
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
    4. Drop the UI written in Dart, rewrite it in Dear Imgui. Use C++26 reflection to get processors and processor options for drag-and-drop graph node editor.
    5. Cache view. A cache miss costs ~100 ns versus a few ns for a hit, so a graph that outgrows the CPU cache gets much slower without any visible sign. Show per-core L1/L2/L3 usage (ring buffers + processor state on that core, against cache sizes read from the CPU) in the graph editor while the user designs the graph, before running it. Sizes are calculated, not measured, so users see when their graph stops fitting.
    6. Bypass toggle for processors with the same input and output type (signal passes through unchanged). Only while the graph is not running.
- **E:** Neuropixels
    1. Until imec provides Linux drivers, probes are run from a separate Windows PC with our own sender program. It uses imec's Windows API to control the probes and streams raw samples to Falcon over Ethernet. Falcon acts as the front end (like OpenEphys): the user manages probes from the Falcon UI, and the sender applies the settings.
    2. Two connections between Falcon and the sender: UDP for data (`electrodePacket`), plain TCP with length-prefixed JSON for control (probe list, IMRO = per-channel electrode site and gain table, start/stop, status).
    3. Samples arrive as raw ADC counts. Falcon converts them to µV and orders channels by depth using the config the sender reports as applied (not the one requested). Config changes are only allowed while streaming is stopped.
    4. IMRO editor in the Falcon UI. NP1 has 960 recording sites on the shank but only 384 channels; each channel N can read site N, N+384 or N+768 (bank 0/1/2), so a channel's number doesn't always match its height on the shank. The editor draws the shank, lets the user mark regions to record from, enforces one site per channel, sets gains, and saves/loads `.imro` files (SpikeGLX format) that are sent to the sender.
