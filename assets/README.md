# Assets

Asset generation will be done after UI is done. This side of the project is parked.

## Processor icons
- Each processor gets one icon.
- Style: an SVG on a 24×24 grid, strokes only (round caps and joins), one colour (`currentColor`), to sit next to the Remix line icons the GUI uses. Stroke width is 2 unless the brochure's stroke switch settles on another.
- Candidates: [processor_icons_brochure.html](processor_icons_brochure.html) holds five per processor. Open it in a browser, click one per row and copy the picks from the bottom bar. Its `icon-data` block is the source of every drawing.
- Once the picks are in, each chosen icon is saved as `icons/<processor file name>.svg` and the brochure is deleted.
