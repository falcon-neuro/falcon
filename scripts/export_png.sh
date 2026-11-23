#!/bin/sh
# Writes the PNG pictures the UI loads, from the SVGs they are drawn in: each processor's icon and
# Falcon's logo. Run by hand when an SVG changes. The PNGs are kept in the repository, so a build
# needs no Inkscape.
set -e
cd "$(dirname "$0")/.."
command -v inkscape
inkscape --version

# An icon is drawn on a grid of 24 and shown 24 pixels wide. Twice as many pixels serve a screen
# that is twice as dense, and the UI makes the smaller sizes from them.
icon_size=48
# The logo's picture is a square with the falcon in its middle, as wide as the square.
logo_size=512
logo_svg=docs/overrides/.icons/falcon/logo.svg

mkdir -p assets/png/processors
for svg in assets/svg/*.svg; do
    name=$(basename "$svg" .svg)
    inkscape "$svg" --export-type=png --export-filename="assets/png/processors/$name.png" \
        --export-width="$icon_size" --export-height="$icon_size"
done
# The SVG's own area, which is wider than high, with as much added above and under it as makes it
# a square. Inkscape counts an area from the top left corner of the SVG's view box.
logo_area=$(sed -n 's/.*viewBox="\([^"]*\)".*/\1/p' "$logo_svg" | head -n 1 |
    awk '{ added = ($3 - $4) / 2; printf "0:%s:%s:%s", -added, $3, $4 + added }')
inkscape "$logo_svg" --export-type=png --export-filename=assets/png/logo.png \
    --export-area="$logo_area" --export-width="$logo_size" --export-height="$logo_size"
ls assets/png assets/png/processors
