"""What the app remembers between two sessions: the sizes of the designer's panels and the
pipeline that is open, kept in the folder .config under the one that holds the pipelines."""
import tempfile
import time
from pathlib import Path

from designer_driver import Designer
from test_welcome_screen import (current_list, state_once_searched, welcome_screen,
                                 write_pipeline)


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


def file_without(path, part):
    """The file's text once it no longer holds this part."""
    end = time.monotonic() + 5
    while time.monotonic() < end:
        if path.exists() and part not in path.read_text():
            return path.read_text()
        time.sleep(0.02)
    raise AssertionError(f"{path} went on holding {part}")


def test_pipeline_that_was_open_is_opened_again_at_the_next_start():
    with tempfile.TemporaryDirectory() as home:
        with Designer(home=home) as designer:
            trigger = designer.add_processor("Trigger", core=0)
            file_with(Path(home) / "falcon" / "prototype_folder" / "pipeline.cpp", "Trigger<")
        write_pipeline(home, "other", "Other")
        # A pipeline that a test named to Falcon isn't remembered: the first start as a user's
        # is on the welcome screen.
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/pipeline/prototype_folder")
            designer.frames(3)
            assert designer.state()["screen"] == "designer"
            file_with(remembered_file(home), "open_pipeline: prototype_folder\n")
        with welcome_screen(home) as designer:
            designer.frames(3)
            assert designer.state()["screen"] == "designer"
            assert set(designer.processors()) == {trigger}
            assert designer.window_title() == "prototype_folder - Falcon"
        # It is opened again as often as Falcon is closed with it open.
        with welcome_screen(home) as designer:
            designer.frames(3)
            assert designer.state()["screen"] == "designer"


def test_start_is_on_the_welcome_screen_after_the_way_back_to_the_list():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/pipeline/memory")
            designer.frames(3)
            file_with(remembered_file(home), "open_pipeline: memory\n")
            designer.click("top_bar/pipelines")
            current_list(designer)
            file_without(remembered_file(home), "open_pipeline")
        with welcome_screen(home) as designer:
            state = state_once_searched(designer)
            assert [pipeline["folder"] for pipeline in state["pipelines"]] == ["memory"]


def test_remembered_pipeline_that_is_gone_or_can_not_be_read_is_forgotten():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "broken", "Broken one", body="this is no pipeline\n")
        outside = Path(home) / "outside"
        outside.mkdir()
        file = remembered_file(home)
        file.parent.mkdir(parents=True)
        # A folder that isn't there, one whose pipeline can't be read, and names that are no
        # folder of the pipelines' directory, as a person or a later Falcon might write them.
        for remembered in ("gone", "broken", "../outside", ".config", "a/b"):
            file.write_text(f"log_panel_height: 300\nopen_pipeline: {remembered}\n")
            with welcome_screen(home) as designer:
                state = state_once_searched(designer)
                assert [pipeline["folder"] for pipeline in state["pipelines"]] == ["broken"]
                assert "open_pipeline" not in file_without(file, "open_pipeline")
            # What else the file remembers stays.
            assert "log_panel_height: 300\n" in file.read_text()
        assert not (Path(home) / "falcon" / "gone").exists()
        assert list(outside.iterdir()) == []
