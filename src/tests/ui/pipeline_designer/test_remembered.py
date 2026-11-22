"""What the app remembers between two sessions: the sizes of the designer's panels, kept in the
folder .config under the one that holds the pipelines."""
import tempfile
import time
from pathlib import Path

from designer_driver import Designer


def remembered_file(home):
    return Path(home) / "falcon" / ".config" / "ui_state.txt"


def file_with(path, part):
    """The file's text once it holds this part: files are written beside the UI's thread."""
    end = time.monotonic() + 5
    while time.monotonic() < end:
        if path.exists() and part in path.read_text():
            return path.read_text()
        time.sleep(0.02)
    raise AssertionError(f"{path} never held {part}")


def drag_edge(designer, edge_name, across, down):
    edge = designer.target(edge_name).point()
    designer.start_drag(edge_name)
    designer.drag_to((edge[0] + across, edge[1] + down))
    designer.drop()
    designer.frames(2)


def test_sizes_of_the_panels_are_written_into_the_config_folder():
    with tempfile.TemporaryDirectory() as home:
        with Designer(home=home) as designer:
            designer.click("bottom_bar/warning")
            designer.frames(3)
        assert "right_panel_width: 360\n" in remembered_file(home).read_text()


def test_sizes_of_the_panels_are_there_again_at_the_next_start():
    with tempfile.TemporaryDirectory() as home:
        with Designer(home=home) as designer:
            designer.click("bottom_bar/warning")
            designer.click("bottom_bar/log")
            drag_edge(designer, "right_panel_edge", across=-130, down=0)
            drag_edge(designer, "log_panel_edge", across=0, down=-90)
            width = designer.target("problems_panel").width
            height = designer.target("log_panel").height
            file_with(remembered_file(home), f"right_panel_width: {round(width)}")
        with Designer(home=home) as designer:
            designer.click("bottom_bar/warning")
            designer.click("bottom_bar/log")
            assert abs(designer.target("problems_panel").width - width) < 2
            assert abs(designer.target("log_panel").height - height) < 2


def test_file_that_a_person_spoilt_does_not_stop_falcon():
    with tempfile.TemporaryDirectory() as home:
        file = remembered_file(home)
        file.parent.mkdir(parents=True)
        file.write_text("right_panel_width: wide\nnonsense\nlog_panel_height: 300\n")
        with Designer(home=home) as designer:
            designer.click("bottom_bar/warning")
            designer.click("bottom_bar/log")
            assert abs(designer.target("problems_panel").width - 360) < 2
            assert abs(designer.target("log_panel").height - 300) < 2
