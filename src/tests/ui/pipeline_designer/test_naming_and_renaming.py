"""A pipeline's name and its folder, when a pipeline is made on the welcome screen and when one is
renamed by the pencil icon on its card: the name is typed in a field, and a line under it says
where the pipeline lies, with a pencil that turns the folder's name into a field."""
import tempfile
from pathlib import Path

from designer_driver import Designer
from test_trash import folders_of, pipeline_with_a_run
from test_welcome_screen import (current_list, file_with, state_once_searched, welcome_screen,
                                 write_pipeline)


def pipeline_file(home, folder):
    return Path(home) / "falcon" / folder / "pipeline.cpp"


def visible_folders(home):
    return sorted(entry.name for entry in (Path(home) / "falcon").iterdir()
                  if not entry.name.startswith("."))


def names_and_folders(state):
    return [(pipeline["name"], pipeline["folder"]) for pipeline in state["pipelines"]]


def shown_path(designer, which):
    """The text of the line under the name's field, of the new pipeline or of the renamed one."""
    return designer.target(f"explorer/{which}_folder_label").note


def type_folder(designer, which, text):
    """Types this in the folder's field, in place of what it holds. The pencil under the name
    shows the field, unless it shows already, still holding the keys. It keeps them."""
    field = f"explorer/{which}_folder"
    designer.click(field if designer.has_target(field) else f"{field}_edit")
    designer.key("Ctrl+A")
    if text:
        designer.type_text(text)
    else:
        designer.key("Delete")


def test_path_of_a_new_pipeline_shows_under_its_name_once_a_name_is_typed():
    with tempfile.TemporaryDirectory() as home:
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            # Before a name is typed there is the name's field and nothing about a folder.
            assert not designer.has_target("explorer/new_folder_label")
            assert not designer.has_target("explorer/new_folder_edit")
            assert not designer.has_target("explorer/new_folder")
            designer.click("explorer/new_name")
            designer.type_text("Memory Impairment, Day 2")
            assert shown_path(designer, "new") == "~/falcon/memory_impairment_day_2"
            assert designer.state()["new_folder"] == "memory_impairment_day_2"
            # The line is a text under the name's field, with a pencil at its right, and no field.
            name = designer.target("explorer/new_name")
            label = designer.target("explorer/new_folder_label")
            assert name.bottom <= label.top and name.left == label.left
            assert label.right <= designer.target("explorer/new_folder_edit").left
            assert not designer.has_target("explorer/new_folder")


def test_pencil_turns_the_folder_into_a_field_which_becomes_a_text_again_when_left():
    with tempfile.TemporaryDirectory() as home:
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/new_name")
            designer.type_text("Memory Impairment, Day 2")
            designer.click("explorer/new_folder_edit")
            # The field stands where the text was, holds the folder, and has the keys.
            assert designer.has_target("explorer/new_folder")
            assert not designer.has_target("explorer/new_folder_label")
            assert designer.state()["new_folder"] == "memory_impairment_day_2"
            designer.key("Ctrl+A")
            designer.type_text("day2")
            assert designer.state()["new_folder"] == "day2"

            # A click elsewhere leaves the field, and the line shows what was typed. A folder of
            # the user's own stays while the name is typed on.
            designer.click("explorer/new_name")
            assert not designer.has_target("explorer/new_folder")
            assert shown_path(designer, "new") == "~/falcon/day2"
            designer.key("End")
            designer.type_text(" again")
            assert shown_path(designer, "new") == "~/falcon/day2"

            # An emptied field follows the name again.
            type_folder(designer, "new", "")
            designer.click("explorer/new_name")
            assert shown_path(designer, "new") == "~/falcon/memory_impairment_day_2_again"


def test_escape_in_the_folders_field_drops_what_was_typed_there_and_nothing_else():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/new_name")
            designer.type_text("Day 2")
            type_folder(designer, "new", "typed_and_dropped")
            designer.key("Escape")
            assert not designer.has_target("explorer/new_folder")
            assert shown_path(designer, "new") == "~/falcon/day_2"
            # The folder still follows the name, as it did before the field was shown.
            designer.click("explorer/new_name")
            designer.key("End")
            designer.type_text("0")
            assert shown_path(designer, "new") == "~/falcon/day_20"

            # In a card that is renamed, the first Escape closes the folder's field and the
            # second one the rename.
            designer.click("explorer/rename/memory")
            type_folder(designer, "rename", "typed_and_dropped")
            designer.key("Escape")
            state = designer.state()
            assert state["asked_to_rename"] == "memory" and state["rename_folder"] == "memory"
            assert shown_path(designer, "rename") == "~/falcon/memory"
            designer.key("Escape")
            assert designer.state()["asked_to_rename"] == ""
        assert visible_folders(home) == ["memory"]


def test_new_pipeline_lies_in_the_folder_that_was_typed_for_it():
    with tempfile.TemporaryDirectory() as home:
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/new_name")
            designer.type_text("Memory Impairment, Day 2")
            type_folder(designer, "new", "day2")
            # Enter in the folder's field makes the pipeline, as in the name's.
            designer.key("Enter")
            designer.frames(3)
            assert designer.state()["screen"] == "designer"
            assert "// Name: Memory Impairment, Day 2\n" in file_with(
                pipeline_file(home, "day2"), "// Name:")
        assert visible_folders(home) == ["day2"]
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            # The row is empty again for the next pipeline.
            assert not designer.has_target("explorer/new_folder_label")


def test_folder_field_takes_letters_digits_and_underscores_only():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/new_name")
            designer.type_text("?!")
            # Such a name gives no folder, and is fine once the user types one.
            assert shown_path(designer, "new") == "~/falcon/"
            designer.click("explorer/create")
            assert designer.state()["new_pipeline_problem"] == "A pipeline needs a folder."
            type_folder(designer, "new", "My pipe-line/../.x_1")
            assert designer.state()["new_folder"] == "Mypipelinex_1"

            type_folder(designer, "new", "memory")
            designer.click("explorer/create")
            state = designer.state()
            assert state["screen"] == "explorer"
            assert state["new_pipeline_problem"] == "The folder memory is there already."

            type_folder(designer, "new", "odd_one")
            designer.click("explorer/create")
            designer.frames(3)
            assert designer.state()["screen"] == "designer"
            assert "// Name: ?!\n" in file_with(pipeline_file(home, "odd_one"), "// Name:")


def test_pencil_on_a_card_shows_its_name_in_a_field_and_its_path_under_it():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory impairment")
        write_pipeline(home, "no_name", "")
        write_pipeline(home, "broken", "Broken one", body="this is no pipeline\n")
        with welcome_screen(home) as designer:
            state = state_once_searched(designer)
            assert state["asked_to_rename"] == ""
            assert not designer.has_target("explorer/rename_name")
            # A pipeline that can't be read isn't renamed.
            assert not designer.has_target("explorer/rename/broken")
            assert (designer.target("explorer/rename/memory").right <=
                    designer.target("explorer/duplicate/memory").left)

            designer.click("explorer/rename/memory")
            state = designer.state()
            # Only asked: the click on the icon didn't open the pipeline.
            assert state["screen"] == "explorer" and state["asked_to_rename"] == "memory"
            assert (state["rename_name"], state["rename_folder"]) == (
                "Memory impairment", "memory")
            assert shown_path(designer, "rename") == "~/falcon/memory"
            assert not designer.has_target("explorer/rename_folder")
            card = designer.target("explorer/pipeline/memory")
            for part in ("explorer/rename_name", "explorer/rename_folder_label",
                         "explorer/rename_folder_edit", "explorer/rename_apply",
                         "explorer/rename_cancel"):
                target = designer.target(part)
                assert card.top <= target.top and target.bottom <= card.bottom
            assert (designer.target("explorer/rename_name").bottom <=
                    designer.target("explorer/rename_folder_label").top)
            # They stand in place of the card's icons.
            assert not designer.has_target("explorer/rename/memory")
            assert not designer.has_target("explorer/move_to_trash/memory")

            # Another card's pencil moves them there; a pipeline without a name is called by its
            # folder.
            designer.click("explorer/rename/no_name")
            state = designer.state()
            assert state["asked_to_rename"] == "no_name"
            assert (state["rename_name"], state["rename_folder"]) == ("no_name", "no_name")
            assert designer.has_target("explorer/rename/memory")


def test_new_name_renames_the_pipeline_and_its_folder_follows_with_its_runs():
    with tempfile.TemporaryDirectory() as home:
        with Designer(home=home) as designer:
            trigger = designer.add_processor("Trigger", core=0)
            file_with(pipeline_file(home, "prototype_folder"), "Trigger<")
        run = Path(home) / "falcon" / "prototype_folder" / "runs" / "2026-11-22_18-00-00"
        run.mkdir(parents=True)
        (run / "recording.bin").write_bytes(b"recorded")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/rename/prototype_folder")
            # The name's field has the keys and its text is selected, so typing replaces it. The
            # folder was the one its name gives, so it follows the new name, which the line shows.
            designer.type_text("Day 2")
            state = designer.state()
            assert (state["rename_name"], state["rename_folder"]) == ("Day 2", "day_2")
            assert shown_path(designer, "rename") == "~/falcon/day_2"
            designer.key("Enter")
            state = current_list(designer)
            assert state["screen"] == "explorer" and state["folder_problem"] == ""
            assert names_and_folders(state) == [("Day 2", "day_2")]
            assert state["asked_to_rename"] == ""
            assert visible_folders(home) == ["day_2"]
            moved_run = Path(home) / "falcon" / "day_2" / "runs" / "2026-11-22_18-00-00"
            assert (moved_run / "recording.bin").read_bytes() == b"recorded"
            text = pipeline_file(home, "day_2").read_text()
            assert "// Name: Day 2\n" in text and "Trigger<" in text
            assert sorted(entry.name for entry in (Path(home) / "falcon" / "day_2").iterdir()) == [
                "pipeline.cpp", "runs"]

            designer.click("explorer/pipeline/day_2")
            designer.frames(3)
            assert designer.state()["screen"] == "designer"
            assert set(designer.processors()) == {trigger}
            assert designer.window_title() == "Day 2 - Falcon"


def test_folder_that_is_not_its_names_stays_when_the_name_changes():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory impairment", "Impairs memory.")
        # A folder made by hand keeps a name that the field wouldn't take.
        write_pipeline(home, "by-hand", "Made by hand")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/rename/memory")
            designer.type_text("Day 3")
            assert shown_path(designer, "rename") == "~/falcon/memory"
            designer.click("explorer/rename_apply")
            state = current_list(designer)
            assert ("Day 3", "memory") in names_and_folders(state)
            text = pipeline_file(home, "memory").read_text()
            assert "// Name: Day 3\n" in text and "// Description: Impairs memory.\n" in text

            designer.click("explorer/rename/by-hand")
            designer.type_text("Renamed")
            designer.key("Enter")
            state = current_list(designer)
            assert state["folder_problem"] == ""
            assert ("Renamed", "by-hand") in names_and_folders(state)


def test_folder_alone_is_renamed_and_the_file_stays_as_it_was():
    with tempfile.TemporaryDirectory() as home:
        pipeline_with_a_run(home, "memory", "Memory")
        text_before = pipeline_file(home, "memory").read_text()
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/rename/memory")
            type_folder(designer, "rename", "memory_2026")
            designer.click("explorer/rename_apply")
            state = current_list(designer)
            assert names_and_folders(state) == [("Memory", "memory_2026")]
        assert visible_folders(home) == ["memory_2026"]
        assert pipeline_file(home, "memory_2026").read_text() == text_before
        assert (Path(home) / "falcon" / "memory_2026" / "runs" / "2026-11-22_18-00-00" /
                "recording.bin").read_bytes() == b"recorded"


def test_rename_is_refused_without_a_name_or_into_a_folder_that_is_there():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory")
        write_pipeline(home, "other", "Other")
        (Path(home) / "falcon" / "notes").mkdir()
        text_before = pipeline_file(home, "memory").read_text()
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/rename/memory")
            for folder, problem in (("other", "The folder other is there already."),
                                    ("notes", "The folder notes is there already.")):
                type_folder(designer, "rename", folder)
                designer.click("explorer/rename_apply")
                state = designer.state()
                assert state["rename_problem"] == problem
                # The card goes on showing its fields.
                assert state["asked_to_rename"] == "memory"
            # Another pipeline's name is no hindrance: two pipelines may be called alike.
            type_folder(designer, "rename", "memory")
            designer.click("explorer/rename_name")
            designer.key("Ctrl+A")
            designer.key("Delete")
            designer.key("Enter")
            state = designer.state()
            assert state["rename_problem"] == "A pipeline needs a name."
            assert folders_of(current_list(designer)) == ["memory", "other"]
        assert visible_folders(home) == ["memory", "notes", "other"]
        assert pipeline_file(home, "memory").read_text() == text_before


def test_cancel_escape_and_an_unchanged_name_rename_nothing():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory")
        file = pipeline_file(home, "memory")
        text_before = file.read_text()
        written_before = file.stat().st_mtime_ns
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/rename/memory")
            designer.type_text("Typed and dropped")
            designer.click("explorer/rename_cancel")
            state = designer.state()
            assert state["asked_to_rename"] == ""
            assert not designer.has_target("explorer/rename_name")

            designer.click("explorer/rename/memory")
            # Asked again, the field holds the pipeline's own name, not the one that was dropped.
            assert designer.state()["rename_name"] == "Memory"
            designer.type_text("Dropped by a key")
            designer.key("Escape")
            assert designer.state()["asked_to_rename"] == ""

            designer.click("explorer/rename/memory")
            designer.key("Enter")
            state = current_list(designer)
            assert state["asked_to_rename"] == "" and state["folder_problem"] == ""
            assert names_and_folders(state) == [("Memory", "memory")]
        assert file.read_text() == text_before and file.stat().st_mtime_ns == written_before


def test_click_on_a_card_that_is_renamed_does_not_open_its_pipeline():
    with tempfile.TemporaryDirectory() as home:
        write_pipeline(home, "memory", "Memory", "A description under the fields.")
        write_pipeline(home, "other", "Other")
        with welcome_screen(home) as designer:
            state_once_searched(designer)
            designer.click("explorer/rename/memory")
            designer.click("explorer/pipeline/memory", across=0.5, down=0.9)
            designer.frames(3)
            state = designer.state()
            assert state["screen"] == "explorer" and state["asked_to_rename"] == "memory"
            # Another card still opens its pipeline, and the fields are gone on the way back.
            designer.click("explorer/pipeline/other")
            designer.frames(3)
            assert designer.state()["screen"] == "designer"
            designer.click("top_bar/pipelines")
            state = current_list(designer)
            assert state["asked_to_rename"] == ""
            assert designer.has_target("explorer/rename/memory")
