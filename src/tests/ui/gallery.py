#!/usr/bin/env python3
"""Writes a PNG picture of every state of Falcon's app that is worth looking at, so that a change
to how the app looks can be judged state by state, before and after.

usage: python3 src/tests/ui/gallery.py <directory> [part of a picture's name ...]

Without name parts every picture is written, and with them those whose name holds one of the
parts. A picture is named by its number, its screen and its state, e.g. 21_welcome_trash_view.png,
and keeps its number whichever pictures are asked for. Each one comes from a Falcon of its own,
whose window isn't shown and whose home directory is its own, brought to its state by the driver
of the tests (pipeline_designer/designer_driver.py). Falcon has to be built first:
sh scripts/make.sh.

One line is printed for each picture. A state that doesn't come up is reported, the others are
still written, and the exit code is then 1.

Nothing here knows a colour, a font or the place of a thing in the window. A thing is found by
the name the app gives it for the driver, and what has no name by what the mouse brings up.
"""
import atexit
import contextlib
import functools
import os
import re
import shutil
import signal
import sys
import tempfile
import time
import traceback
from pathlib import Path
from typing import NamedTuple

UI_TESTS_DIRECTORY = Path(__file__).resolve().parent
sys.path.insert(0, str(UI_TESTS_DIRECTORY / "pipeline_designer"))
# These are imported after the path they are found on.
from designer_driver import LEFT_BUTTON, OUTPUT_DIRECTORY, Designer, port  # noqa: E402
from screenshot import read_ppm, write_png  # noqa: E402
from test_adding import cores_listed_in  # noqa: E402
from test_remembered import drag_edge  # noqa: E402
from test_run import SECONDS_FOR_A_BUILD, wait_until  # noqa: E402
from test_welcome_screen import current_list, welcome_screen, write_pipeline  # noqa: E402

# A tooltip shows once the mouse has rested on its item for a moment, which the app counts in
# frames while the driver is on. These are more than that moment takes.
FRAMES_FOR_A_TOOLTIP = 30
# Off the window, where nothing is under the mouse.
MOUSE_AWAY = (-100.0, -100.0)
# How often a state that lasts milliseconds is brought about before it counts as not caught.
ATTEMPTS_AT_A_PASSING_STATE = 8

# The pictures in their order, each as its screen and the function that takes it.
PICTURES = []


def picture_of(screen):
    """Marks a function as one picture of this screen. The function is named after the state it
    shows, and takes the file to write the picture to."""
    def add(take_picture):
        PICTURES.append((screen, take_picture))
        return take_picture
    return add


welcome_picture = picture_of("welcome")
designer_picture = picture_of("designer")


# ------------------------------------------------------------------ the mouse and the window

def mouse_away(designer):
    """Takes the mouse off the window, so that a picture shows nothing as being under it."""
    designer.move_to(MOUSE_AWAY)
    designer.frames(3)


def mouse_over(designer, name, across=0.5, down=0.5):
    """Rests the mouse on the named thing until its tooltip shows, if it has one."""
    designer.move_to(designer.target(name).point(across, down))
    designer.frames(FRAMES_FOR_A_TOOLTIP)


def picture_at_rest(designer, picture_path):
    mouse_away(designer)
    designer.picture(picture_path)


def window_of(designer):
    """The window as it is drawn now: its width, its height and its pixels, three bytes each, row
    after row from the top."""
    with tempfile.TemporaryDirectory() as directory:
        ppm_path = Path(directory) / "falcon.ppm"
        designer.command(f"picture {ppm_path}")
        return read_ppm(ppm_path)


def points_between(left, top, right, bottom, step_across, step_down):
    """Points inside a rectangle, row after row from the top, or from the bottom where step_down
    is negative. No point lies on the edge the rows start from."""
    first_row = int(top if step_down > 0 else bottom) + step_down
    for down in range(first_row, int(bottom if step_down > 0 else top), step_down):
        for across in range(int(left), int(right), step_across):
            yield (across, down)


def first_point_with_a_tooltip(designer, points, lit_by_the_mouse=()):
    """The first of these points over which the mouse brings up a tooltip, for a thing that has
    one and no name to find it by. It is told as an eye tells it: the window with the mouse there
    differs from the window with the mouse away. `lit_by_the_mouse` names what lies near the
    points and only lights up under the mouse, which doesn't count. Leaves the mouse at the
    point, with the tooltip showing."""
    mouse_away(designer)
    windows_without_a_tooltip = [window_of(designer)]
    for name in lit_by_the_mouse:
        mouse_over(designer, name)
        windows_without_a_tooltip.append(window_of(designer))
    for point in points:
        designer.move_to(point)
        designer.frames(FRAMES_FOR_A_TOOLTIP)
        if window_of(designer) not in windows_without_a_tooltip:
            return point
    raise AssertionError("no tooltip showed at any of the places tried")


def pixels_around(window, target, margin):
    """The pixels of a window that show the target and a margin around it."""
    width, height, pixels = window
    left = max(0, int(target.left) - margin)
    right = min(width, int(target.right) + margin)
    rows = range(max(0, int(target.top) - margin), min(height, int(target.bottom) + margin))
    return b"".join(pixels[(row * width + left) * 3:(row * width + right) * 3] for row in rows)


# ------------------------------------------------------------------ the welcome screen

BROKEN_PIPELINE = "this is no pipeline\n"
TRASHED_WITH_RUNS = "2026-11-20_09-15-42_ripple_detection"
TRASHED_WITHOUT_RUNS = "2026-11-18_17-02-10_pilot"


def write_some_pipelines(home):
    """Five pipelines: with a short description, with one of several lines, without one, without
    a name, and one whose pipeline.cpp can't be read."""
    write_pipeline(home, "memory_impairment", "Memory impairment",
                   "Detects ripples in CA1 and triggers the stimulation that disrupts them.")
    write_pipeline(home, "spike_recording", "Spike recording",
                   "Detects spikes on 384 channels of a Neuropixels probe and records every one "
                   "of them with the signal around it, to sort them after the session. The "
                   "recorder runs on a core that isn't realtime, so that a slow disk never holds "
                   "a detector up.")
    write_pipeline(home, "latency_benchmark", "Latency benchmark")
    write_pipeline(home, "test_bench", "")
    write_pipeline(home, "closed_loop_day_3", "Closed loop, day 3",
                   "Stimulates on every second ripple.", body=BROKEN_PIPELINE)


def write_trashed_pipeline(home, trash_name, name, description="", run_count=0):
    directory = write_pipeline(home, f".trash/{trash_name}", name, description)
    for run_number in range(1, run_count + 1):
        (directory / "runs" / f"2026-11-{run_number:02d}_10-00-00").mkdir(parents=True)


def write_some_trashed_pipelines(home):
    """Three pipelines in the trash: with runs, without any, and one that somebody put there by
    hand, whose folder doesn't say when."""
    write_trashed_pipeline(home, TRASHED_WITH_RUNS, "Ripple detection",
                           "The first try of the ripple detector on a recorded signal.",
                           run_count=3)
    write_trashed_pipeline(home, TRASHED_WITHOUT_RUNS, "Pilot")
    write_trashed_pipeline(home, "old_tests", "Old tests", run_count=1)


def write_pipelines_and_trash(home):
    write_some_pipelines(home)
    write_some_trashed_pipelines(home)


MORE_PIPELINES = (
    ("Ripple detection", "Detects sharp-wave ripples in a signal filtered to the ripple band."),
    ("Theta phase stimulation", ""),
    ("Replay decoding", "Decodes the animal's position from the spikes of 64 tetrodes."),
    ("Spike detection, 32 channels", ""),
    ("Spike detection, 384 channels", ""),
    ("Burst detection", "Counts the spikes of each channel and triggers on a burst."),
    ("Signal generator test", ""),
    ("Filter comparison", "The same signal through an FIR filter and an IIR filter."),
    ("Recorder test", ""),
    ("Round trip, one channel", "Measures the delay from the electrode to the pulse."),
    ("Round trip, every channel", ""),
    ("Sleep session", "Ripple detection for a whole night."),
    ("Awake session", ""),
    ("Stimulation artifact", "Pauses the detector while the stimulator pulses."),
    ("Level crossing demo", ""),
    ("Event logger demo", "Logs an event a second."),
)


def write_many_pipelines(home):
    """More pipelines than the window has room for, so that their list scrolls. Each lies in the
    folder that the welcome screen would name after it."""
    write_pipelines_and_trash(home)
    for name, description in MORE_PIPELINES:
        folder_name = re.sub("[^a-z0-9]+", "_", name.lower()).strip("_")
        write_pipeline(home, folder_name, name, description)


@contextlib.contextmanager
def welcome_screen_of(prepare_home=None):
    """Falcon on its welcome screen, once that lists what `prepare_home` put into the home
    directory."""
    with tempfile.TemporaryDirectory(prefix="falcon_gallery_home_") as home:
        if prepare_home is not None:
            prepare_home(home)
        with welcome_screen(home) as designer:
            current_list(designer)
            yield designer


def pipeline_that_is_a_named_pipe(home):
    """Puts a folder into the home whose pipeline.cpp is a named pipe, and returns the pipe. The
    welcome screen's search reads every pipeline.cpp to its end, and this one's end comes only
    when whoever writes to it closes it: the search waits there (see held_search)."""
    folder = Path(home) / "falcon" / "held_by_the_gallery"
    folder.mkdir(parents=True)
    pipe_path = folder / "pipeline.cpp"
    os.mkfifo(pipe_path)
    return pipe_path


@contextlib.contextmanager
def held_search(designer, pipe_path):
    """Holds the welcome screen's search for the pipelines for as long as the with statement
    lasts, so that a picture shows what the screen shows meanwhile. The search must be on its way
    or about to start, and is held at the named pipe. It must end before Falcon is closed, which
    waits for it."""
    writing_end = None
    try:
        end = time.monotonic() + 5
        while writing_end is None:
            designer.frames(1)
            try:
                # Opens only once the search reads the pipe, and keeps it reading until closed.
                writing_end = os.open(pipe_path, os.O_WRONLY | os.O_NONBLOCK)
            except OSError:
                if time.monotonic() > end:
                    raise AssertionError("the search never read the named pipe") from None
                time.sleep(0.01)
        yield
    finally:
        if writing_end is not None:
            os.close(writing_end)
        pipe_path.unlink()
        pipe_path.parent.rmdir()


def scroll_list_to_its_end(designer):
    """Drags the scrollbar of the welcome screen's list down as far as it goes. The scrollbar has
    no name: it stands between the cards' right edge and the edge of the column that holds them,
    which lies as far from the window's right edge as the cards from its left one."""
    window_width, window_height, _ = window_of(designer)
    cards = [target for name, target in designer.targets().items()
             if name.startswith("explorer/pipeline/")]
    top_card = min(cards, key=lambda card: card.top)
    scrollbar_middle = (top_card.right + window_width - top_card.left) / 2
    # While the list isn't scrolled, the scrollbar's grab starts at the column's top, so it is
    # beside the field for a new pipeline's name.
    designer.move_to((scrollbar_middle, designer.target("explorer/new_name").point()[1]))
    designer.frames(1)
    designer.command(f"mouse_down {LEFT_BUTTON}")
    designer.move_in_steps_to((scrollbar_middle, window_height))
    designer.command(f"mouse_up {LEFT_BUTTON}")
    designer.frames(3)
    if designer.target(top_card.name).top >= top_card.top:
        raise AssertionError("the list of pipelines didn't scroll")


@welcome_picture
def no_pipelines(picture_path):
    with welcome_screen_of() as designer:
        picture_at_rest(designer, picture_path)


@welcome_picture
def looking_for_pipelines(picture_path):
    with tempfile.TemporaryDirectory(prefix="falcon_gallery_home_") as home:
        write_pipelines_and_trash(home)
        pipe_path = pipeline_that_is_a_named_pipe(home)
        with welcome_screen(home) as designer, held_search(designer, pipe_path):
            picture_at_rest(designer, picture_path)
            assert not designer.state()["has_searched"], "the search had ended"


@welcome_picture
def pipelines(picture_path):
    with welcome_screen_of(write_pipelines_and_trash) as designer:
        picture_at_rest(designer, picture_path)


@welcome_picture
def many_pipelines(picture_path):
    with welcome_screen_of(write_many_pipelines) as designer:
        picture_at_rest(designer, picture_path)


@welcome_picture
def many_pipelines_scrolled_to_the_end(picture_path):
    with welcome_screen_of(write_many_pipelines) as designer:
        scroll_list_to_its_end(designer)
        picture_at_rest(designer, picture_path)


@welcome_picture
def pipeline_with_a_long_name(picture_path):
    def write_pipelines_and_one_with_a_long_name(home):
        write_pipelines_and_trash(home)
        write_pipeline(home, "ripple_disruption_animal_12_session_3",
                       "Closed-loop ripple disruption with optogenetic stimulation in dorsal CA1, "
                       "animal 12, session 3 of the pilot", "Stimulates on every ripple.")

    with welcome_screen_of(write_pipelines_and_one_with_a_long_name) as designer:
        picture_at_rest(designer, picture_path)


@welcome_picture
def card_under_the_mouse(picture_path):
    with welcome_screen_of(write_pipelines_and_trash) as designer:
        # Left of the card's icons, each of which is a thing under the mouse of its own.
        mouse_over(designer, "explorer/pipeline/memory_impairment", across=0.3)
        designer.picture(picture_path)


@welcome_picture
def rename_icon_with_its_tooltip(picture_path):
    with welcome_screen_of(write_pipelines_and_trash) as designer:
        mouse_over(designer, "explorer/rename/memory_impairment")
        designer.picture(picture_path)


@welcome_picture
def duplicate_icon_with_its_tooltip(picture_path):
    with welcome_screen_of(write_pipelines_and_trash) as designer:
        mouse_over(designer, "explorer/duplicate/memory_impairment")
        designer.picture(picture_path)


@welcome_picture
def trash_icon_with_its_tooltip(picture_path):
    with welcome_screen_of(write_pipelines_and_trash) as designer:
        mouse_over(designer, "explorer/move_to_trash/memory_impairment")
        designer.picture(picture_path)


@welcome_picture
def new_name_typed(picture_path):
    with welcome_screen_of(write_pipelines_and_trash) as designer:
        designer.click("explorer/new_name")
        designer.type_text("Ripple detection, day 2")
        picture_at_rest(designer, picture_path)


@welcome_picture
def new_folder_pencil_with_its_tooltip(picture_path):
    with welcome_screen_of(write_pipelines_and_trash) as designer:
        designer.click("explorer/new_name")
        designer.type_text("Ripple detection, day 2")
        mouse_over(designer, "explorer/new_folder_edit")
        designer.picture(picture_path)


@welcome_picture
def new_folder_field(picture_path):
    with welcome_screen_of(write_pipelines_and_trash) as designer:
        designer.click("explorer/new_name")
        designer.type_text("Ripple detection, day 2")
        designer.click("explorer/new_folder_edit")
        designer.key("Ctrl+A")
        designer.type_text("ripples_2")
        picture_at_rest(designer, picture_path)


@welcome_picture
def new_name_refused(picture_path):
    with welcome_screen_of(write_pipelines_and_trash) as designer:
        designer.click("explorer/new_name")
        designer.type_text("Memory impairment")
        designer.click("explorer/create")
        assert designer.state()["new_pipeline_problem"], "the name wasn't refused"
        picture_at_rest(designer, picture_path)


@welcome_picture
def card_being_renamed(picture_path):
    with welcome_screen_of(write_pipelines_and_trash) as designer:
        designer.click("explorer/rename/memory_impairment")
        picture_at_rest(designer, picture_path)


@welcome_picture
def card_being_renamed_with_its_folder_field(picture_path):
    with welcome_screen_of(write_pipelines_and_trash) as designer:
        designer.click("explorer/rename/memory_impairment")
        designer.type_text("Memory impairment, day 2")
        designer.click("explorer/rename_folder_edit")
        picture_at_rest(designer, picture_path)


@welcome_picture
def rename_refused(picture_path):
    with welcome_screen_of(write_pipelines_and_trash) as designer:
        designer.click("explorer/rename/memory_impairment")
        # Another pipeline's name, whose folder is there already.
        designer.type_text("Spike recording")
        designer.click("explorer/rename_apply")
        assert designer.state()["rename_problem"], "the rename wasn't refused"
        picture_at_rest(designer, picture_path)


@welcome_picture
def move_to_the_trash_failed(picture_path):
    def write_pipelines_and_a_file_in_place_of_the_trash(home):
        write_some_pipelines(home)
        (Path(home) / "falcon" / ".trash").write_text("A file, where the trash's folder goes.\n")

    with welcome_screen_of(write_pipelines_and_a_file_in_place_of_the_trash) as designer:
        designer.click("explorer/move_to_trash/latency_benchmark")
        assert current_list(designer)["folder_problem"], "the pipeline was moved"
        picture_at_rest(designer, picture_path)


@welcome_picture
def busy(picture_path):
    with welcome_screen_of(write_pipelines_and_trash) as designer:
        pipe_path = pipeline_that_is_a_named_pipe(designer.home)
        # The copy is made, and the search for the pipelines that follows it is held: the list
        # isn't current meanwhile, and nothing in it can be clicked.
        designer.click("explorer/duplicate/latency_benchmark")
        with held_search(designer, pipe_path):
            picture_at_rest(designer, picture_path)
            assert not designer.state()["is_list_current"], "the search had ended"


@welcome_picture
def trash_button_with_its_tooltip(picture_path):
    with welcome_screen_of(write_pipelines_and_trash) as designer:
        mouse_over(designer, "explorer/open_trash")
        designer.picture(picture_path)


@contextlib.contextmanager
def trash_view_of(prepare_home):
    with welcome_screen_of(prepare_home) as designer:
        designer.click("explorer/open_trash")
        yield designer


@welcome_picture
def trash_view(picture_path):
    with trash_view_of(write_pipelines_and_trash) as designer:
        picture_at_rest(designer, picture_path)


@welcome_picture
def trash_view_back_button_with_its_tooltip(picture_path):
    with trash_view_of(write_pipelines_and_trash) as designer:
        mouse_over(designer, "trash/back")
        designer.picture(picture_path)


@welcome_picture
def trash_view_question_before_deleting_for_good(picture_path):
    with trash_view_of(write_pipelines_and_trash) as designer:
        designer.click(f"trash/delete/{TRASHED_WITH_RUNS}")
        picture_at_rest(designer, picture_path)


@welcome_picture
def trash_view_restore_refused(picture_path):
    def write_a_folder_in_the_place_of_a_trashed_pipeline(home):
        write_pipelines_and_trash(home)
        write_pipeline(home, "ripple_detection", "Ripple detection, the second")

    with trash_view_of(write_a_folder_in_the_place_of_a_trashed_pipeline) as designer:
        designer.click(f"trash/restore/{TRASHED_WITH_RUNS}")
        assert current_list(designer)["folder_problem"], "the pipeline was restored"
        picture_at_rest(designer, picture_path)


@welcome_picture
def trash_view_empty(picture_path):
    def write_one_trashed_pipeline(home):
        write_some_pipelines(home)
        write_trashed_pipeline(home, TRASHED_WITHOUT_RUNS, "Pilot")

    with trash_view_of(write_one_trashed_pipeline) as designer:
        designer.click(f"trash/restore/{TRASHED_WITHOUT_RUNS}")
        assert current_list(designer)["trash"] == [], "the pipeline wasn't restored"
        picture_at_rest(designer, picture_path)


# ------------------------------------------------------------------ the designer

PIPELINE_NAME = "Memory impairment"


@contextlib.contextmanager
def designer_of_an_empty_pipeline():
    """Falcon on the designer of a pipeline that has a name and holds nothing yet."""
    with tempfile.TemporaryDirectory(prefix="falcon_gallery_home_") as home:
        write_pipeline(home, "prototype_folder", PIPELINE_NAME)
        with Designer(home=home) as designer:
            yield designer


class Example(NamedTuple):
    """The names of the example pipeline's processors."""
    generator: str
    detector: str
    trigger: str
    parallel_output: str
    recorder: str


def add_example_pipeline(designer):
    """The pipeline of the app's own picture (add_example_pipeline in pipeline_designer.cpp),
    which the driver can't ask for, so it is built here: a generator on core 0, a detector, a
    trigger and an output in a chain on core 1, and a recorder on core 2."""
    example = Example(
        generator=designer.add_processor("SignalGenerator", core=0),
        detector=designer.add_processor("SpikeDetector", core=1),
        trigger=designer.add_processor("Trigger", core=1),
        parallel_output=designer.add_processor("PCIeParallelOutput", core=1),
        recorder=designer.add_processor("Recorder", core=2))
    designer.connect((example.generator, "output"), (example.detector, "input"))
    designer.connect((example.detector, "output"), (example.trigger, "input"))
    designer.connect((example.trigger, "output"), (example.parallel_output, "input"))
    designer.connect((example.trigger, "output"), (example.recorder, "inputs"))
    return example


@contextlib.contextmanager
def designer_of_the_example_pipeline():
    with designer_of_an_empty_pipeline() as designer:
        yield designer, add_example_pipeline(designer)


def select(designer, processor):
    """Opens a processor's options in the panel at the right, by a click on its block's name."""
    designer.click(f"block/{processor}/handle")


def type_in_place_of(designer, field, text, across=0.5):
    """Types this into the named field in place of what it holds, and leaves it being typed in.
    A number's box is clicked left of its middle: its two step buttons are at its right end."""
    designer.click(field, across=across)
    designer.key("Ctrl+A")
    designer.type_text(text)


@designer_picture
def empty_pipeline(picture_path):
    with designer_of_an_empty_pipeline() as designer:
        picture_at_rest(designer, picture_path)


@designer_picture
def example_pipeline(picture_path):
    with designer_of_the_example_pipeline() as (designer, _):
        picture_at_rest(designer, picture_path)


def panel_entry_under(designer, name):
    """The entry of the processors panel that stands right under the named one."""
    entry = designer.target(name)
    entries_under = [target for target_name, target in designer.targets().items()
                     if target_name.startswith("panel/") and target.top > entry.top]
    return min(entries_under, key=lambda target: target.top)


@designer_picture
def processors_panel_entry_under_the_mouse(picture_path):
    with designer_of_an_empty_pipeline() as designer:
        mouse_over(designer, "panel/SpikeDetector")
        designer.picture(picture_path)


@designer_picture
def processors_panel_entry_opened(picture_path):
    with designer_of_an_empty_pipeline() as designer:
        designer.click("panel/SpikeDetector")
        picture_at_rest(designer, picture_path)


@designer_picture
def processors_panel_info_tooltip(picture_path):
    with designer_of_an_empty_pipeline() as designer:
        designer.click("panel/SpikeDetector")
        opened = designer.target("panel/SpikeDetector")
        entry_under = panel_entry_under(designer, "panel/SpikeDetector")
        # An "(i)" has no name. The last one stands right of a connection's name, in the last
        # line but one of what the opened entry shows: the lines are tried from the bottom, each
        # along the panel's left half.
        first_point_with_a_tooltip(
            designer,
            points_between(opened.left, opened.bottom, opened.point()[0], entry_under.top,
                           step_across=8, step_down=-6),
            lit_by_the_mouse=("panel/SpikeDetector", entry_under.name))
        designer.picture(picture_path)


@designer_picture
def processors_panel_that_scrolls(picture_path):
    with designer_of_an_empty_pipeline() as designer:
        designer.click("panel/SignalGenerator")
        designer.click("panel/RippleDetector")
        picture_at_rest(designer, picture_path)


@designer_picture
def block_option_line_with_its_tooltip(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        mouse_over(designer, f"block/{example.generator}/option/frequency")
        designer.picture(picture_path)


@designer_picture
def block_stream_line_with_its_tooltip(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        mouse_over(designer, f"block/{example.generator}/stream")
        designer.picture(picture_path)


@designer_picture
def block_remove_cross_with_its_tooltip(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        mouse_over(designer, f"block/{example.trigger}/remove")
        designer.picture(picture_path)


@designer_picture
def port_with_its_tooltip(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        mouse_over(designer, port(example.detector, "input", "input"))
        designer.picture(picture_path)


def move_under_the_box_of_core(designer, core, across):
    """Moves the mouse to a free place under a core's box, where the line that follows it from a
    picked port shows whole."""
    box = designer.target(f"core/{core}")
    designer.move_to((box.point(across)[0], box.bottom + box.height / 2))
    designer.frames(3)


@designer_picture
def output_picked(picture_path):
    with designer_of_an_empty_pipeline() as designer:
        generator = designer.add_processor("SignalGenerator", core=0)
        detector = designer.add_processor("SpikeDetector", core=1)
        designer.add_processor("Trigger", core=1)
        designer.add_processor("EventLogger", core=1)
        designer.connect((generator, "output"), (detector, "input"))
        designer.click(port(detector, "output", "output"))
        move_under_the_box_of_core(designer, 1, across=0.5)
        designer.picture(picture_path)


@designer_picture
def input_picked(picture_path):
    with designer_of_an_empty_pipeline() as designer:
        generator = designer.add_processor("SignalGenerator", core=0)
        detector = designer.add_processor("SpikeDetector", core=1)
        trigger = designer.add_processor("Trigger", core=1)
        designer.add_processor("EventLogger", core=1)
        designer.connect((generator, "output"), (detector, "input"))
        designer.click(port(trigger, "input", "input"))
        # Not straight under the picked input, where the line would stand upright in its block.
        move_under_the_box_of_core(designer, 1, across=0.9)
        designer.picture(picture_path)


@designer_picture
def greyed_port_with_its_tooltip(picture_path):
    with designer_of_an_empty_pipeline() as designer:
        ripple_detector = designer.add_processor("RippleDetector", core=0)
        trigger = designer.add_processor("Trigger", core=0)
        designer.add_processor("EventLogger", core=0)
        # A trigger's input takes no ripple events, and says so while their output is picked.
        designer.click(port(ripple_detector, "output", "output"))
        mouse_over(designer, port(trigger, "input", "input"))
        designer.picture(picture_path)


@designer_picture
def processor_dragged_from_the_panel_over_a_core(picture_path):
    with designer_of_the_example_pipeline() as (designer, _):
        # A second chain beside the first, so that a drop place stands between two chains too.
        designer.add_processor("LatencyBenchmark", core=1)
        designer.drag_over_core("panel/EventLogger", core=1, across=0.8)
        designer.picture(picture_path)


@designer_picture
def processor_dragged_from_the_panel_over_an_empty_core(picture_path):
    with designer_of_the_example_pipeline() as (designer, _):
        # In the box's lower right corner: the dragged processor's name hangs under the mouse,
        # and at the box's middle it would lie over the words of the drop place.
        designer.drag_over_core("panel/EventLogger", core=3, across=0.8, down=0.8)
        designer.picture(picture_path)


@designer_picture
def block_dragged_over_another_core(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        designer.drag_over_core(f"block/{example.trigger}/handle", core=0, across=0.8)
        designer.picture(picture_path)


@designer_picture
def isolated_cores_line_with_its_tooltip(picture_path):
    with designer_of_the_example_pipeline() as (designer, _):
        box = designer.target("core/0")
        # The line has no name. It is what stands above the boxes, from their left edge on.
        across = box.left + box.width / 10
        first_point_with_a_tooltip(designer, points_between(
            across, designer.target("top_bar/pipelines").bottom, across + 1, box.top,
            step_across=1, step_down=-4))
        designer.picture(picture_path)


def lowest_core_that_is_not_isolated():
    isolated_cores = cores_listed_in("/sys/devices/system/cpu/isolated")
    return min(core for core in cores_listed_in("/sys/devices/system/cpu/present")
               if core not in isolated_cores)


def hold_drag_over_core(designer, source, core):
    """Starts to drag the named thing and holds it over a core's box. A box that lies beyond the
    window's right edge is brought into view first: the cores' area scrolls sideways while a
    drag is held in the strip along its edge (scroll_while_dragging_at_an_edge in
    pipeline_designer.cpp), which is the window's edge while no panel is open at the right."""
    start = designer.target(source).point()
    designer.start_drag(source)
    window_width, _, _ = window_of(designer)
    # Inside that strip, and where a box is clear of it.
    place_that_scrolls = (window_width - 20, start[1])
    clear_of_the_strip = window_width - 80
    box = designer.target(f"core/{core}")
    while box.right > clear_of_the_strip:
        designer.move_to(place_that_scrolls)
        designer.frames(1)
        scrolled_box = designer.target(f"core/{core}")
        if scrolled_box.left >= box.left:
            break  # the area is scrolled as far as it goes
        box = scrolled_box
    designer.drag_to(designer.target(f"core/{core}").point())


def drop_into_core(designer, source, core):
    """Drags the named thing into a core, to the last of its drop places, scrolling to the
    core's box if need be."""
    hold_drag_over_core(designer, source, core)
    designer.drag_to(designer.drop_places()[-1].point())
    designer.drop()


def add_processor_to_any_core(designer, type_name, core):
    """As the driver's add_processor, for a core whose box may lie beyond the window's edge."""
    names_before = set(designer.processors())
    drop_into_core(designer, f"panel/{type_name}", core)
    (new_name,) = set(designer.processors()) - names_before
    return new_name


@contextlib.contextmanager
def designer_with_a_recorder_where_the_os_runs():
    """A generator and a detector on cores 0 and 1 and their recorder on the lowest core that
    isn't isolated, with the window showing that core. Yields the designer and the recorder."""
    with designer_of_an_empty_pipeline() as designer:
        its_core = lowest_core_that_is_not_isolated()
        generator = designer.add_processor("SignalGenerator", core=0)
        detector = designer.add_processor("SpikeDetector", core=1)
        designer.connect((generator, "output"), (detector, "input"))
        # A drag shows the box of one more core than is in use, and two ports are connected
        # while both are in view. So the recorder starts beside the detector and gets to a core
        # further on one core at a time.
        first_core = min(its_core, 2)
        recorder = designer.add_processor("Recorder", core=first_core)
        designer.connect((detector, "output"), (recorder, "inputs"))
        for core in range(first_core + 1, its_core + 1):
            drop_into_core(designer, f"block/{recorder}/handle", core)
        assert designer.processors()[recorder]["core"] == its_core
        yield designer, recorder


@designer_picture
def core_not_isolated(picture_path):
    with designer_with_a_recorder_where_the_os_runs() as (designer, _):
        picture_at_rest(designer, picture_path)


@designer_picture
def core_not_isolated_with_its_tooltip(picture_path):
    with designer_with_a_recorder_where_the_os_runs() as (designer, recorder):
        box = designer.target(f"core/{designer.processors()[recorder]['core']}")
        # The mark has no name. It stands right of the core's name, above the core's blocks.
        first_point_with_a_tooltip(designer, points_between(
            box.left, box.top, box.right, designer.target(f"block/{recorder}").top,
            step_across=int(box.width / 10), step_down=4))
        designer.picture(picture_path)


@designer_picture
def connections_of_every_kind(picture_path):
    with designer_of_an_empty_pipeline() as designer:
        generator = designer.add_processor("SignalGenerator", core=0)
        signal_filter = designer.add_processor("IirFilter", core=1)
        detector = designer.add_processor("RippleDetector", core=1)
        # Samples from one core to the next, and inside a core.
        designer.connect((generator, "output"), (signal_filter, "input"))
        designer.connect((signal_filter, "output"), (detector, "input"))
        # Events inside a core, into a port that takes any number of connections.
        logger = designer.add_processor("EventLogger", core=1)
        designer.connect((detector, "output"), (logger, "inputs"))
        second_detector = designer.add_processor("RippleDetector", core=2)
        recorder = designer.add_processor("Recorder", core=2)
        designer.connect((signal_filter, "output"), (second_detector, "input"))
        # A state value from one core to the next.
        designer.connect((detector, "is_in_lockout"), (second_detector, "is_detection_enabled"))
        # Samples and events, from its own core and from others, into one port.
        designer.connect((second_detector, "output"), (recorder, "inputs"))
        designer.connect((detector, "output"), (recorder, "inputs"))
        designer.connect((generator, "output"), (recorder, "inputs"))
        picture_at_rest(designer, picture_path)


def add_wide_pipeline(designer):
    """A chain over six cores, a processor on each, which is wider than the window. Two
    processors are connected while both are in view."""
    generator = add_processor_to_any_core(designer, "SignalGenerator", 0)
    signal_filter = add_processor_to_any_core(designer, "IirFilter", 1)
    designer.connect((generator, "output"), (signal_filter, "input"))
    detector = add_processor_to_any_core(designer, "SpikeDetector", 2)
    designer.connect((signal_filter, "output"), (detector, "input"))
    trigger = add_processor_to_any_core(designer, "Trigger", 3)
    designer.connect((detector, "output"), (trigger, "input"))
    parallel_output = add_processor_to_any_core(designer, "PCIeParallelOutput", 4)
    designer.connect((trigger, "output"), (parallel_output, "input"))
    recorder = add_processor_to_any_core(designer, "Recorder", 5)
    designer.connect((trigger, "output"), (recorder, "inputs"))
    designer.connect((detector, "output"), (recorder, "inputs"))


@designer_picture
def pipeline_wider_than_the_window(picture_path):
    with tempfile.TemporaryDirectory(prefix="falcon_gallery_home_") as home:
        write_pipeline(home, "prototype_folder", PIPELINE_NAME)
        with Designer(home=home) as designer:
            add_wide_pipeline(designer)
        # Opened again, the pipeline shows from its left end.
        with Designer(home=home) as designer:
            assert len(designer.processors()) == 6, "the pipeline wasn't kept whole"
            picture_at_rest(designer, picture_path)


@designer_picture
def pipeline_wider_than_the_window_scrolled_to_its_end(picture_path):
    with designer_of_an_empty_pipeline() as designer:
        add_wide_pipeline(designer)
        picture_at_rest(designer, picture_path)


@designer_picture
def block_selected_with_its_options(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        select(designer, example.detector)
        picture_at_rest(designer, picture_path)


@designer_picture
def options_step_button_under_the_mouse(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        select(designer, example.detector)
        # The two buttons are squares at the right end of the number's box, plus the last.
        box = designer.target("options_panel/input/threshold")
        designer.move_to((box.right - box.height / 2, box.point()[1]))
        designer.frames(FRAMES_FOR_A_TOOLTIP)
        designer.picture(picture_path)


@designer_picture
def options_changed_value_and_default_button(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        select(designer, example.detector)
        type_in_place_of(designer, "options_panel/input/threshold", "-42.5", across=0.25)
        designer.key("Enter")
        assert designer.has_target("options_panel/default/threshold")
        picture_at_rest(designer, picture_path)


@designer_picture
def options_stream_and_closed_list(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        select(designer, example.generator)
        picture_at_rest(designer, picture_path)


@designer_picture
def options_list_open(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        select(designer, example.generator)
        designer.click("options_panel/input/waveform")
        mouse_over(designer, "options_panel/choice/waveform/Square")
        designer.picture(picture_path)


@designer_picture
def options_text_field(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        select(designer, example.recorder)
        picture_at_rest(designer, picture_path)


@designer_picture
def options_text_half_typed_and_not_saved(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        select(designer, example.recorder)
        # A text without its closing quote can't be read back, so the bar says it isn't saved.
        type_in_place_of(designer, "options_panel/input/file_name", '"session_12')
        picture_at_rest(designer, picture_path)
        assert designer.state()["keeping_note"], "the bar had no note about saving"


@designer_picture
def options_processor_without_options(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        select(designer, example.trigger)
        picture_at_rest(designer, picture_path)


@designer_picture
def options_renamed_output_and_own_name_button(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        select(designer, example.detector)
        type_in_place_of(designer, "options_panel/output_name/output", "spikes")
        designer.key("Enter")
        assert designer.has_target("options_panel/own_output_name/output")
        picture_at_rest(designer, picture_path)


@designer_picture
def options_output_name_used_twice(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        select(designer, example.trigger)
        type_in_place_of(designer, "options_panel/output_name/output",
                         f"{example.detector}_output")
        designer.key("Enter")
        assert designer.has_target("options_panel/output_name_shared/output")
        picture_at_rest(designer, picture_path)


@designer_picture
def panels_at_other_sizes(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        select(designer, example.detector)
        designer.click("bottom_bar/log")
        drag_edge(designer, "right_panel_edge", across=-200, down=0)
        drag_edge(designer, "log_panel_edge", across=0, down=-160)
        picture_at_rest(designer, picture_path)


@designer_picture
def options_panel_at_its_narrowest(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        select(designer, example.generator)
        # Further than the panel gives way.
        drag_edge(designer, "right_panel_edge", across=designer.target("options_panel").width,
                  down=0)
        picture_at_rest(designer, picture_path)


@designer_picture
def panel_edge_under_the_mouse(picture_path):
    with designer_of_the_example_pipeline() as (designer, example):
        select(designer, example.detector)
        mouse_over(designer, "right_panel_edge")
        designer.picture(picture_path)


@designer_picture
def top_bar_pipelines_button_with_its_tooltip(picture_path):
    with designer_of_the_example_pipeline() as (designer, _):
        mouse_over(designer, "top_bar/pipelines")
        designer.picture(picture_path)


@designer_picture
def top_bar_compile_button_under_the_mouse(picture_path):
    with designer_of_the_example_pipeline() as (designer, _):
        mouse_over(designer, "top_bar/compile")
        designer.picture(picture_path)


@designer_picture
def top_bar_compile_button_pressed(picture_path):
    with designer_of_the_example_pipeline() as (designer, _):
        designer.move_to(designer.target("top_bar/compile").point())
        designer.frames(1)
        designer.command(f"mouse_down {LEFT_BUTTON}")
        designer.frames(2)
        designer.picture(picture_path)
        # Let go beside the button, which then isn't clicked.
        designer.move_to(MOUSE_AWAY)
        designer.command(f"mouse_up {LEFT_BUTTON}")


@designer_picture
def top_bar_disabled_run_button_under_the_mouse(picture_path):
    with designer_of_the_example_pipeline() as (designer, _):
        mouse_over(designer, "top_bar/run")
        designer.picture(picture_path)


@designer_picture
def bottom_bar_warning_with_its_tooltip(picture_path):
    with designer_of_the_example_pipeline() as (designer, _):
        mouse_over(designer, "bottom_bar/warning")
        designer.picture(picture_path)


@designer_picture
def bottom_bar_log_button_with_its_tooltip(picture_path):
    with designer_of_the_example_pipeline() as (designer, _):
        mouse_over(designer, "bottom_bar/log")
        designer.picture(picture_path)


@designer_picture
def log_panel_before_any_run(picture_path):
    with designer_of_the_example_pipeline() as (designer, _):
        designer.click("bottom_bar/log")
        picture_at_rest(designer, picture_path)


@designer_picture
def problems_panel_without_problems(picture_path):
    with designer_of_the_example_pipeline() as (designer, _):
        designer.click("bottom_bar/warning")
        picture_at_rest(designer, picture_path)


# ------------------------------------------------------------------ compiling and running

def build_state(designer):
    return designer.state()["build"]["state"]


def run_state(designer):
    return designer.state()["run"]["state"]


def bar_text(designer):
    """What the bar at the bottom said of the pipeline in the frame drawn last, which is the
    picture's when a picture was taken last."""
    return designer.state()["bar"]


def wait_for_the_build(designer):
    wait_until(designer, lambda: build_state(designer) != "running",
               "the end of the compilation", SECONDS_FOR_A_BUILD)
    return build_state(designer)


def add_generator_into_logger(designer):
    """An EventGenerator into an EventLogger on core 0: a pipeline that compiles within seconds
    and, run, writes ten lines a second into its log."""
    generator = designer.add_processor("EventGenerator", core=0)
    logger = designer.add_processor("EventLogger", core=0)
    designer.connect((generator, "output"), (logger, "inputs"))
    select(designer, generator)
    type_in_place_of(designer, "options_panel/input/rate", "10", across=0.25)
    designer.key("Enter")
    designer.click("options_panel/close")


@functools.cache
def home_with_a_compiled_pipeline():
    """A home directory whose pipeline is compiled. It is made at the first call, and every
    state that needs a compiled pipeline starts on a copy of it: a compilation takes seconds."""
    home = Path(tempfile.mkdtemp(prefix="falcon_gallery_compiled_"))
    atexit.register(shutil.rmtree, home, ignore_errors=True)
    write_pipeline(home, "prototype_folder", PIPELINE_NAME)
    with Designer(home=home) as designer:
        add_generator_into_logger(designer)
        designer.click("top_bar/compile")
        assert wait_for_the_build(designer) == "succeeded", "the pipeline didn't compile"
    return home


@contextlib.contextmanager
def designer_of_a_compiled_pipeline(prepare_home=None):
    """Falcon on the designer of a pipeline that is compiled, in a home directory of its own that
    `prepare_home` may change before Falcon starts."""
    with tempfile.TemporaryDirectory(prefix="falcon_gallery_home_") as home:
        shutil.copytree(home_with_a_compiled_pipeline(), home, dirs_exist_ok=True)
        if prepare_home is not None:
            prepare_home(Path(home))
        with Designer(home=home) as designer:
            assert bar_text(designer) == "Compiled", "the copied pipeline isn't compiled"
            yield designer


def start_run(designer):
    """Starts the compiled pipeline and waits until its log holds some of its logger's lines."""
    designer.click("top_bar/run")
    wait_until(designer, lambda: run_state(designer) == "running", "the pipeline running")
    wait_until(designer,
               lambda: sum(" log event_logger: " in line
                           for line in designer.state()["run"]["lines"]) >= 12,
               "lines of the event logger")


def wait_for_the_end_of_the_run(designer):
    wait_until(designer, lambda: run_state(designer) not in ("starting", "running", "stopping"),
               "the end of the run")


def click_and_take_picture(designer, name, picture_path):
    """Clicks the named thing, takes the mouse off it and takes the picture at once, for a state
    that lasts milliseconds. Whoever calls this then asks whether the picture's frame showed the
    state (see bar_text)."""
    designer.move_to(designer.target(name).point())
    designer.frames(1)
    designer.command(f"mouse_down {LEFT_BUTTON}")
    designer.command(f"mouse_up {LEFT_BUTTON}")
    designer.move_to(MOUSE_AWAY)
    designer.picture(picture_path)


def kill_pipeline_of(designer):
    """Kills the pipeline that Falcon runs, as a crash would end it: whatever runs from under
    the home directory."""
    home = str(designer.home.resolve()) + os.sep
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                if os.readlink(entry / "exe").startswith(home):
                    os.kill(int(entry.name), signal.SIGKILL)
            except OSError:
                continue  # gone meanwhile, or another user's


def picture_of_the_strongest_mark(designer, picture_path, block_name, window_before):
    """Takes the picture in which a block's mark shows strongest. The mark of a block with a
    problem comes and goes, so the window is looked at again and again over a few seconds of the
    app's time, and the look in which the block differs most from `window_before`, taken before
    it was marked, is the picture."""
    frames_between_looks = 4
    look_count = 60
    block = designer.target(block_name)
    margin = int(block.height / 8)
    block_before = pixels_around(window_before, block, margin)
    strongest_window = None
    largest_difference = -1
    for _ in range(look_count):
        designer.frames(frames_between_looks - 1)
        window = window_of(designer)
        difference = sum(abs(now - before) for now, before in zip(
            pixels_around(window, block, margin), block_before))
        if difference > largest_difference:
            largest_difference = difference
            strongest_window = window
    write_png(Path(picture_path), *strongest_window)


@designer_picture
def compiling(picture_path):
    with designer_of_an_empty_pipeline() as designer:
        add_generator_into_logger(designer)
        designer.click("top_bar/compile")
        picture_at_rest(designer, picture_path)
        assert bar_text(designer) == "Compiling", "the compilation had ended"
        wait_for_the_build(designer)


@designer_picture
def compiled(picture_path):
    with designer_of_a_compiled_pipeline() as designer:
        picture_at_rest(designer, picture_path)


@designer_picture
def top_bar_run_button_under_the_mouse(picture_path):
    with designer_of_a_compiled_pipeline() as designer:
        mouse_over(designer, "top_bar/run")
        designer.picture(picture_path)


@designer_picture
def compile_failed_faulty_block_marked(picture_path):
    with designer_of_an_empty_pipeline() as designer:
        # The example pipeline without the connection into its detector, which the designer
        # itself refuses to write.
        generator = designer.add_processor("SignalGenerator", core=0)
        detector = designer.add_processor("SpikeDetector", core=1)
        trigger = designer.add_processor("Trigger", core=1)
        recorder = designer.add_processor("Recorder", core=2)
        designer.connect((detector, "output"), (trigger, "input"))
        designer.connect((trigger, "output"), (recorder, "inputs"))
        mouse_away(designer)
        window_before = window_of(designer)
        designer.click("top_bar/compile")
        mouse_away(designer)
        problems = designer.state()["build"]["problems"]
        assert bar_text(designer) == "Compile failed" and problems, "the compilation didn't fail"
        faulty = problems[0]["processor"]
        assert faulty in (generator, detector), "the problem names no processor"
        picture_of_the_strongest_mark(designer, picture_path, f"block/{faulty}", window_before)


@contextlib.contextmanager
def designer_after_errors_of_the_compiler():
    """A recorder whose file's name was typed without its quotes, which the designer writes and
    the compiler refuses with several errors. Yields the designer, the recorder's block and the
    window as it was before the compilation."""
    with designer_of_an_empty_pipeline() as designer:
        generator = designer.add_processor("SignalGenerator", core=0)
        recorder = designer.add_processor("Recorder", core=1)
        designer.connect((generator, "output"), (recorder, "inputs"))
        select(designer, recorder)
        type_in_place_of(designer, "options_panel/input/file_name", "recording.bin")
        designer.key("Enter")
        designer.click("options_panel/close")
        mouse_away(designer)
        window_before = window_of(designer)
        designer.click("top_bar/compile")
        assert wait_for_the_build(designer) == "failed", "the compilation didn't fail"
        mouse_away(designer)
        yield designer, f"block/{recorder}", window_before


@designer_picture
def compile_failed_compiler_errors(picture_path):
    with designer_after_errors_of_the_compiler() as (designer, faulty_block, window_before):
        picture_of_the_strongest_mark(designer, picture_path, faulty_block, window_before)


@designer_picture
def compile_failed_compiler_text(picture_path):
    with designer_after_errors_of_the_compiler() as (designer, faulty_block, window_before):
        designer.click("problems_panel/raw")
        mouse_away(designer)
        picture_of_the_strongest_mark(designer, picture_path, faulty_block, window_before)


@designer_picture
def run_starting(picture_path):
    with designer_of_a_compiled_pipeline() as designer:
        for _ in range(ATTEMPTS_AT_A_PASSING_STATE):
            click_and_take_picture(designer, "top_bar/run", picture_path)
            if bar_text(designer) == "Starting":
                return
            # The pipeline was running before the picture's frame: once more.
            wait_until(designer, lambda: run_state(designer) == "running", "the pipeline running")
            designer.click("top_bar/run")
            wait_for_the_end_of_the_run(designer)
        raise AssertionError("the run was never caught while it started")


@designer_picture
def running(picture_path):
    with designer_of_a_compiled_pipeline() as designer:
        start_run(designer)
        picture_at_rest(designer, picture_path)
        assert bar_text(designer) == "Running", "the run had ended"


@designer_picture
def top_bar_stop_button_under_the_mouse(picture_path):
    with designer_of_a_compiled_pipeline() as designer:
        start_run(designer)
        mouse_over(designer, "top_bar/run")
        designer.picture(picture_path)
        assert bar_text(designer) == "Running", "the run had ended"


@designer_picture
def run_stopping(picture_path):
    with designer_of_a_compiled_pipeline() as designer:
        for _ in range(ATTEMPTS_AT_A_PASSING_STATE):
            start_run(designer)
            click_and_take_picture(designer, "top_bar/run", picture_path)
            is_caught = bar_text(designer) == "Stopping"
            wait_for_the_end_of_the_run(designer)
            if is_caught:
                return
        raise AssertionError("the run was never caught while it stopped")


@designer_picture
def stopped_after_a_run(picture_path):
    with designer_of_a_compiled_pipeline() as designer:
        start_run(designer)
        designer.click("top_bar/run")
        wait_for_the_end_of_the_run(designer)
        assert run_state(designer) == "none", "the run didn't end well"
        picture_at_rest(designer, picture_path)


@designer_picture
def start_failed(picture_path):
    def take_the_right_to_run_it(home):
        (home / "falcon" / "prototype_folder" / "pipeline").chmod(0o644)

    with designer_of_a_compiled_pipeline(take_the_right_to_run_it) as designer:
        designer.click("top_bar/run")
        assert bar_text(designer) == "Start failed", "the pipeline started"
        picture_at_rest(designer, picture_path)


@designer_picture
def run_failed(picture_path):
    with designer_of_a_compiled_pipeline() as designer:
        start_run(designer)
        kill_pipeline_of(designer)
        wait_for_the_end_of_the_run(designer)
        assert bar_text(designer) == "Run failed", "the run didn't count as failed"
        picture_at_rest(designer, picture_path)


# ------------------------------------------------------------------ all of them

def main():
    arguments = sys.argv[1:]
    if not arguments or any(argument.startswith("-") for argument in arguments):
        sys.exit(__doc__)
    directory = Path(arguments[0]).resolve()
    name_parts = arguments[1:]
    wanted_pictures = []
    for number, (screen, take_picture) in enumerate(PICTURES, start=1):
        file_name = f"{number:02d}_{screen}_{take_picture.__name__}.png"
        if not name_parts or any(part in file_name for part in name_parts):
            wanted_pictures.append((file_name, take_picture))
    if not wanted_pictures:
        sys.exit(f"no picture's name holds any of: {' '.join(name_parts)}")
    if not (OUTPUT_DIRECTORY / "falcon").exists():
        sys.exit(f"{OUTPUT_DIRECTORY / 'falcon'} is missing: build Falcon first "
                 "(sh scripts/make.sh)")
    directory.mkdir(parents=True, exist_ok=True)
    failed_count = 0
    start_of_all = time.monotonic()
    for file_name, take_picture in wanted_pictures:
        picture_path = directory / file_name
        # A picture of an earlier run must not pass for this run's.
        picture_path.unlink(missing_ok=True)
        start = time.monotonic()
        try:
            take_picture(picture_path)
            if not picture_path.exists():
                raise AssertionError("no picture was taken")
        except Exception:  # a state may fail to come up in any way, and the others still do
            picture_path.unlink(missing_ok=True)
            failed_count += 1
            print(f"FAILED {picture_path}")
            print("    " + traceback.format_exc().rstrip().replace("\n", "\n    "))
            continue
        print(f"{picture_path} ({time.monotonic() - start:.1f} s)")
    print(f"{len(wanted_pictures) - failed_count} pictures written, {failed_count} failed, "
          f"in {time.monotonic() - start_of_all:.0f} s")
    sys.exit(1 if failed_count else 0)


if __name__ == "__main__":
    main()
