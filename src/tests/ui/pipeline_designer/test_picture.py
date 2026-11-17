"""A picture of the window, which is how the designer is looked at without a screen."""
import struct
import tempfile
from pathlib import Path

from designer_driver import Designer


def test_picture_of_the_window_is_written_as_a_png():
    with Designer() as designer, tempfile.TemporaryDirectory() as directory:
        designer.add_processor("Trigger", core=0)
        picture = Path(directory) / "designer.png"
        designer.picture(picture)
        data = picture.read_bytes()
        assert data[:8] == b"\x89PNG\r\n\x1a\n"
        width, height = struct.unpack(">II", data[16:24])
        assert (width, height) == (1600, 1000)
