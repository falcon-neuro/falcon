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
- Each processor gets one icon.
- Style: an SVG on a 24×24 grid, strokes only (round caps and joins), one colour (`currentColor`), to sit next to the Remix line icons the GUI uses. Stroke width is 2 unless the brochure's stroke switch settles on another.
- Candidates: [assets/processor_icons_brochure.html](../../assets/processor_icons_brochure.html) holds five per processor. Open it in a browser, click one per row and copy the picks from the bottom bar. Its `icon-data` block is the source of every drawing.
- Once the picks are in, each chosen icon is saved as `assets/icons/<processor file name>.svg` and the brochure is deleted.

## Open questions
- Should an About page list the third-party licences? Those of the libraries (ImGui, GLFW, glad, liburing, Ripple Neuromed's xipplib) are not checked yet.
