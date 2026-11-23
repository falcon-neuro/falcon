# Assets

Asset generation will be done after UI is done. This side of the project is parked.

## UI icons
- **Font**: `assets/remixicon.ttf`, Remix Icon 4.8.0, 3,188 icons, Apache 2.0.
- **Pinned to 4.8.0**: later releases are not open source, and `list_icons.py` refuses them.
- **Use**: `icon("delete-bin-line")`, from `src/ui/common/icons.cpp`, is a character of every UI font, with the size and colour of the text around it. A wrong name doesn't compile.
- **Browse**: open `assets/remixicon_showcase.html` in a browser. A click copies an icon's `icon("...")` code.
- **Replacing the font**: run `src/ui/common/list_icons.py`, which rewrites `icon_list.cpp` and the showcase page.

## Font licences
- Each font's licence text ships beside it: `assets/remixicon_license.txt` (Apache 2.0) and `assets/inter_license.txt` (SIL Open Font License 1.1). Both fonts may ship with Falcon's GPL v3, and nothing has to be shown on screen.
- The fonts are unmodified. A changed font takes on more duties.
- The brand logos among the icons are their owners' trademarks, only to point at that brand.

## Processor icons
- **The app loads each icon as a PNG** made from its SVG by `scripts/export_png.sh`, as it does Falcon's logo (see **Step 18** in ui_design.md). The script reads the SVGs from `assets/svg/`, where they lie today, not from the `assets/icons/` named below.
- Each processor has one icon, `assets/icons/<processor file name>.svg`. All 17 were saved on 23 November 2026, once the user had chosen them ("we are done now, save all the svgs").
- Style: an SVG on a 24×24 grid, strokes only (round caps and joins), one colour (`currentColor`), to sit next to the Remix line icons the GUI uses. Stroke width is 1 (decided by the user on 23 November 2026, over the 2 the candidates were drawn at: "1 instead"). The brochure's stroke switch still shows the other widths.
- **Four processors share one icon, the Ethernet port**: NeuralynxInput, NeuralynxStreamer, NeuropixelsInput and RippleNeuromedInput (decided by the user on 23 November 2026: "nlx input neuropixels input and ripple neuromed input should have the ethernet svg", then "NeuralynxStreamer gets ethernet icon too"). Each of the four has a file of its own with the same drawing, so that every processor's icon is found by its file name (Claude's choice, not confirmed by the user).
- **Picked by the user on 23 November 2026**, each from five candidates:

  | Processor | Icon |
  | --- | --- |
  | EventLogger | Terminal |
  | FirFilter | Windowed taps |
  | IirFilter | Feedback path |
  | LatencyBenchmark | Stopwatch |
  | LevelCrossingDetector | Crossing point |
  | SpikeDetector | Magnified spike |
  | RippleDetector | Ripple burst |
  | Trigger | Channels merging |
  | SignalGenerator | Source symbol |
  | Recorder | Floppy disk |
  | PCIeParallelOutput | D-sub connector |
  | Gain, the template processor | Amplifier |
  | EventGenerator | Tuning fork, a stand-in |

- **EventGenerator's tuning fork is a stand-in** (the user on 23 November 2026: "let's go with tuning fork for now, we will replace it later"). What the user was shown before, for whoever draws its replacement:
  1. Metronome, ticks on a time axis, repeating bolt, emitter, alarm clock: "not good enough, try again, ticks on time axis was a bit good but not enough".
  2. Impulses on a time axis, pulse source, clock over its ticks, clock signal, events along a timeline, all built on ticks in time: "none, try again", with no reason.
  3. Bolts on a time axis, clock with a bolt, recurring clock, bolt source, tuning fork: five different ideas, since the reason for the second rejection wasn't known.
- The brochure: [assets/processor_icons_brochure.html](../../assets/processor_icons_brochure.html) shows every saved icon at several sizes, in both themes, and holds five candidates for each processor that is open, none at present. It is kept for EventGenerator's replacement (Claude's choice: the plan had been to delete it once the icons were saved). Its `icon-data` block holds a copy of each drawing, so a change to an icon is made in both.
- Choosing with the brochure: open it in a browser. On each open row, click one candidate, or press **Reject all five** and write what needs to be improved (asked for by the user on 23 November 2026). Then copy from the bottom bar: the picks on one line, then one line for each rejected row with its reason. A rejected row gets five new candidates, drawn from its reason.

## Open questions
- Should the filled dots of two processor icons shrink? The crossing point of LevelCrossingDetector and the pins of PCIeParallelOutput were drawn for stroke 2 and kept their size, so they look heavy beside lines of stroke 1 (seen by Claude on 23 November 2026 in a picture of the saved files).
- Is the brochure deleted once EventGenerator has its final icon?
- Should an About page list the third-party licences? Those of the libraries (ImGui, GLFW, glad, liburing, Ripple Neuromed's xipplib) are not checked yet.
