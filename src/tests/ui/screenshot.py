#!/usr/bin/env python3
"""Writes a PNG picture of Falcon's UI, drawn into a window that isn't shown.

usage: python3 src/tests/ui/screenshot.py <picture.png> [--empty | --selected | --welcome]

By default the pipeline designer holds a small example pipeline. --empty leaves it empty,
--selected also opens the options of one processor, and --welcome shows the welcome screen, which
Falcon starts on, without a pipeline. Falcon has to be built first.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[3]
OUTPUT_DIRECTORY = REPOSITORY / "build" / "output"
TIME_LIMIT_SECONDS = 30


def read_ppm(path):
    data = path.read_bytes()
    # "P6", the width, the height and the highest value, each followed by one white space.
    fields = data.split(maxsplit=4)
    if len(fields) != 5 or fields[0] != b"P6" or fields[3] != b"255":
        raise ValueError("not the PPM picture Falcon writes")
    width, height = int(fields[1]), int(fields[2])
    pixels = data[len(data) - width * height * 3:]
    return width, height, pixels


def write_png(path, width, height, pixels):
    def chunk(kind, content):
        checksum = zlib.crc32(kind + content) & 0xFFFFFFFF
        return struct.pack(">I", len(content)) + kind + content + struct.pack(">I", checksum)

    row_size = width * 3
    # Each row starts with the number of its filter, 0 for none.
    rows = b"".join(b"\x00" + pixels[row * row_size:(row + 1) * row_size] for row in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
                     chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def main():
    arguments = sys.argv[1:]
    options = [argument for argument in arguments if argument.startswith("--")]
    paths = [argument for argument in arguments if not argument.startswith("--")]
    if len(paths) != 1 or any(option not in ("--empty", "--selected", "--welcome") for option in options):
        sys.exit(__doc__)
    picture_path = Path(paths[0]).resolve()

    environment = dict(os.environ)
    environment.pop("FALCON_EXAMPLE_PIPELINE", None)
    environment.pop("FALCON_OPEN_PIPELINE", None)
    with tempfile.TemporaryDirectory() as directory:
        ppm_path = Path(directory) / "falcon.ppm"
        environment["FALCON_SCREENSHOT"] = str(ppm_path)
        # Falcon lists the pipelines it keeps under the home directory: a picture shows none of
        # the user's.
        environment["HOME"] = directory
        if "--empty" in options:
            environment["FALCON_OPEN_PIPELINE"] = str(Path(directory) / "falcon" / "a_pipeline")
        elif "--welcome" not in options:
            environment["FALCON_EXAMPLE_PIPELINE"] = "selected" if "--selected" in options else "1"
        subprocess.run([str(OUTPUT_DIRECTORY / "falcon")], cwd=OUTPUT_DIRECTORY, env=environment,
                       timeout=TIME_LIMIT_SECONDS, check=True, stdout=subprocess.DEVNULL)
        width, height, pixels = read_ppm(ppm_path)
    write_png(picture_path, width, height, pixels)
    print(f"{picture_path} ({width} x {height})")


if __name__ == "__main__":
    main()
