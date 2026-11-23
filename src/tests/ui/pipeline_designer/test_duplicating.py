"""Duplicating a pipeline from the welcome screen: an icon on its card makes a copy at once, a new
pipeline in the list that holds what the first one holds, without its runs. Nothing is opened."""
import tempfile
from pathlib import Path

from designer_driver import Designer
from test_trash import folders_of
from test_welcome_screen import (current_list, file_with, state_once_searched, welcome_screen,
                                 write_pipeline)


def pipeline_file(home, folder):
    return Path(home) / "falcon" / folder / "pipeline.cpp"


def visible_folders(home):
    return sorted(entry.name for entry in (Path(home) / "falcon").iterdir()
                  if not entry.name.startswith("."))


def test_icon_on_a_pipeline_makes_a_copy_with_its_processors_and_without_its_runs():
    with tempfile.TemporaryDirectory() as home:
        with Designer(home=home) as designer:
            trigger = designer.add_processor("Trigger", core=0)
            file_with(pipeline_file(home, "prototype_folder"), "Trigger<")
        run = Path(home) / "falcon" / "prototype_folder" / "runs" / "2026-11-22_18-00-00"
        run.mkdir(parents=True)
        (run / "recording.bin").write_bytes(b"recorded")
        text_before = pipeline_file(home, "prototype_folder").read_text()
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/duplicate/prototype_folder")
            state = current_list(designer)
            # The copy is a new card in the list, and nothing was opened. A pipeline without a
            # name is called by its folder, and its copy after that.
            assert state["screen"] == "explorer" and state["folder_problem"] == ""
            assert [(pipeline["folder"], pipeline["name"], pipeline["problem"])
                    for pipeline in state["pipelines"]] == [
                        ("prototype_folder", "", ""),
                        ("prototype_folder_copy", "prototype_folder copy", "")]
            copy_text = pipeline_file(home, "prototype_folder_copy").read_text()
            assert "// Name: prototype_folder copy\n" in copy_text and "Trigger<" in copy_text
            assert visible_folders(home) == ["prototype_folder", "prototype_folder_copy"]
            assert [entry.name for entry in pipeline_file(
                home, "prototype_folder_copy").parent.iterdir()] == ["pipeline.cpp"]
            # The first one is as it was.
            assert pipeline_file(home, "prototype_folder").read_text() == text_before
            assert (run / "recording.bin").read_bytes() == b"recorded"

            # A click opens the copy, as any other pipeline, with what the first one holds.
            designer.click("explorer/pipeline/prototype_folder_copy")
            designer.frames(3)
            assert designer.state()["screen"] == "designer"
            assert set(designer.processors()) == {trigger}
            assert designer.window_title() == "prototype_folder copy - Falcon"


def test_copy_is_named_after_its_pipeline_and_keeps_the_description():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory impairment, day 1", "Impairs memory.")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/duplicate/memory")
            state = current_list(designer)
            # The copy's folder is named after the copy's name, not after the first one's folder.
            assert state["pipelines"] == [
                {"folder": "memory", "name": "Memory impairment, day 1",
                 "description": "Impairs memory.", "problem": ""},
                {"folder": "memory_impairment_day_1_copy",
                 "name": "Memory impairment, day 1 copy", "description": "Impairs memory.",
                 "problem": ""},
            ]


def test_second_copy_and_a_copy_of_a_copy_each_get_a_name_of_their_own():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/duplicate/memory")
            current_list(designer)
            designer.click("explorer/duplicate/memory")
            state = current_list(designer)
            assert [(pipeline["folder"], pipeline["name"]) for pipeline in state["pipelines"]] == [
                ("memory", "Memory"), ("memory_copy", "Memory copy"),
                ("memory_copy_2", "Memory copy 2")]
            designer.click("explorer/duplicate/memory_copy")
            state = current_list(designer)
            assert ("memory_copy_copy", "Memory copy copy") in [
                (pipeline["folder"], pipeline["name"]) for pipeline in state["pipelines"]]


def test_copy_gets_the_first_number_whose_folder_is_free():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory")
        write_pipeline(home, "memory_copy", "Memory copy")
        # A folder that is no pipeline's takes its name all the same.
        (Path(home) / "falcon" / "memory_copy_2").mkdir()
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/duplicate/memory")
            state = current_list(designer)
            assert folders_of(state) == ["memory", "memory_copy", "memory_copy_3"]
            assert state["pipelines"][2]["name"] == "Memory copy 3"
        assert not (Path(home) / "falcon" / "memory_copy_2" / "pipeline.cpp").exists()


def test_copy_is_refused_once_every_numbered_folder_is_taken():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory")
        (Path(home) / "falcon" / "memory_copy").mkdir()
        for number in range(2, 100):
            (Path(home) / "falcon" / f"memory_copy_{number}").mkdir()
        folders_before = visible_folders(home)
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/duplicate/memory")
            state = current_list(designer)
            assert state["screen"] == "explorer" and folders_of(state) == ["memory"]
            assert state["folder_problem"] == "The folder memory_copy_99 is there already."
        assert visible_folders(home) == folders_before
        assert not (Path(home) / "falcon" / "memory_copy_99" / "pipeline.cpp").exists()


def test_pipeline_that_cannot_be_read_has_no_copy_and_one_in_the_trash_neither():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "broken", "Broken one", body="this is no pipeline\n")
        write_pipeline(home, "memory", "Memory")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            assert not designer.has_target("explorer/duplicate/broken")
            assert designer.has_target("explorer/move_to_trash/broken")
            assert designer.has_target("explorer/duplicate/memory")
            # The two icons of a card lie side by side, the copy's first.
            assert (designer.target("explorer/duplicate/memory").right <=
                    designer.target("explorer/move_to_trash/memory").left)

            designer.click("explorer/move_to_trash/memory")
            assert folders_of(current_list(designer)) == ["broken"]
            designer.click("explorer/open_trash")
            assert designer.state()["is_trash_shown"]
            assert not any("duplicate" in name for name in designer.targets())
