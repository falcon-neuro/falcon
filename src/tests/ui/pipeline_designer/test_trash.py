"""Deleting a pipeline from the welcome screen: it is moved into the trash, which the screen
shows too, and from there brought back or deleted for good."""
import tempfile
import time
from pathlib import Path

from designer_driver import Designer
from test_welcome_screen import state_once_searched, welcome_screen, write_pipeline


def current_list(designer):
    """The welcome screen's state once its list shows the folders as they are: what is moved or
    deleted, and the search after it, run beside the UI's thread."""
    end = time.monotonic() + 5
    while time.monotonic() < end:
        designer.frames(1)
        state = designer.state()
        if state["screen"] == "explorer" and state["is_list_current"]:
            return state
        time.sleep(0.02)
    raise AssertionError("the welcome screen's list never became current")


def folders_of(state):
    return [pipeline["folder"] for pipeline in state["pipelines"]]


def trash_folder(home):
    return Path(home) / "falcon" / ".trash"


def pipeline_with_a_run(home, folder, name):
    """A pipeline's folder with one run in it, which holds a recording."""
    directory = write_pipeline(home, folder, name)
    run = directory / "runs" / "2026-11-22_18-00-00"
    run.mkdir(parents=True)
    (run / "recording.bin").write_bytes(b"recorded")
    return directory


def test_no_trash_is_shown_while_nothing_was_deleted():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory")
        with welcome_screen(home) as designer:
            state = state_once_searched(designer)
            assert state["trash"] == [] and not designer.has_target("explorer/open_trash")
            assert designer.has_target("explorer/move_to_trash/memory")


def test_icon_on_a_pipeline_moves_it_into_the_trash_with_all_it_holds():
    with tempfile.TemporaryDirectory() as home:
        pipeline_with_a_run(home, "memory", "Memory")
        write_pipeline(home, "other", "Other")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/move_to_trash/memory")
            state = current_list(designer)
            # Only moved: the click on the icon didn't open the pipeline either.
            assert state["screen"] == "explorer" and folders_of(state) == ["other"]
            assert [(trashed["folder"], trashed["name"], trashed["runs"])
                    for trashed in state["trash"]] == [("memory", "Memory", 1)]
            in_trash = trash_folder(home) / state["trash"][0]["trash_name"]
            assert state["trash"][0]["trash_name"].endswith("_memory")
            assert (in_trash / "runs" / "2026-11-22_18-00-00" / "recording.bin").read_bytes() == (
                b"recorded")
            assert not (Path(home) / "falcon" / "memory").exists()
            assert designer.has_target("explorer/open_trash")


def test_trash_is_opened_from_the_list_and_left_again():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/move_to_trash/memory")
            state = current_list(designer)
            trash_name = state["trash"][0]["trash_name"]
            designer.click("explorer/open_trash")
            assert designer.state()["is_trash_shown"]
            assert designer.has_target(f"trash/pipeline/{trash_name}")
            assert not designer.has_target("explorer/new_name")
            designer.click("trash/back")
            assert not designer.state()["is_trash_shown"]
            assert designer.has_target("explorer/new_name")


def test_pipeline_is_brought_back_from_the_trash_as_it_was():
    with tempfile.TemporaryDirectory() as home:
        pipeline_with_a_run(home, "memory", "Memory")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/move_to_trash/memory")
            trash_name = current_list(designer)["trash"][0]["trash_name"]
            designer.click("explorer/open_trash")
            designer.click(f"trash/restore/{trash_name}")
            state = current_list(designer)
            assert state["trash"] == [] and folders_of(state) == ["memory"]
            restored = Path(home) / "falcon" / "memory"
            assert (restored / "runs" / "2026-11-22_18-00-00" / "recording.bin").exists()
            assert not (trash_folder(home) / trash_name).exists()


def test_pipeline_is_not_brought_back_over_a_folder_of_its_name():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory, the first")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/move_to_trash/memory")
            trash_name = current_list(designer)["trash"][0]["trash_name"]
            write_pipeline(home, "memory", "Memory, the second")
            designer.click("explorer/open_trash")
            designer.click(f"trash/restore/{trash_name}")
            state = current_list(designer)
            assert state["folder_problem"] == "A folder named memory is there already."
            assert [trashed["name"] for trashed in state["trash"]] == ["Memory, the first"]
            assert "Memory, the second" in (
                Path(home) / "falcon" / "memory" / "pipeline.cpp").read_text()


def test_delete_for_good_asks_first_and_then_deletes_the_pipeline_with_its_runs():
    with tempfile.TemporaryDirectory() as home:
        pipeline_with_a_run(home, "memory", "Memory")
        write_pipeline(home, "other", "Other")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/move_to_trash/memory")
            trash_name = current_list(designer)["trash"][0]["trash_name"]
            in_trash = trash_folder(home) / trash_name
            designer.click("explorer/open_trash")

            # The first click only asks, and Keep takes the question back.
            assert not designer.has_target(f"trash/confirm/{trash_name}")
            designer.click(f"trash/delete/{trash_name}")
            designer.frames(3)
            assert in_trash.exists() and designer.has_target(f"trash/confirm/{trash_name}")
            designer.click(f"trash/keep/{trash_name}")
            assert in_trash.exists() and not designer.has_target(f"trash/confirm/{trash_name}")

            designer.click(f"trash/delete/{trash_name}")
            designer.click(f"trash/confirm/{trash_name}")
            state = current_list(designer)
            assert state["trash"] == [] and state["folder_problem"] == ""
            assert not in_trash.exists()
            # Nothing else went with it.
            assert (Path(home) / "falcon" / "other" / "pipeline.cpp").exists()
            assert trash_folder(home).exists()


def test_two_pipelines_of_one_name_lie_in_the_trash_apart():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory, the first")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/move_to_trash/memory")
            current_list(designer)
        # A folder's name in the trash holds the time to the second.
        time.sleep(1.1)
        write_pipeline(home, "memory", "Memory, the second")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/move_to_trash/memory")
            state = current_list(designer)
            # The last one deleted comes first.
            assert [trashed["name"] for trashed in state["trash"]] == [
                "Memory, the second", "Memory, the first"]


def test_pipeline_that_cannot_be_read_is_moved_into_the_trash_too():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "broken", "Broken one", body="this is no pipeline\n")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/move_to_trash/broken")
            state = current_list(designer)
            assert folders_of(state) == []
            assert [trashed["name"] for trashed in state["trash"]] == ["Broken one"]
