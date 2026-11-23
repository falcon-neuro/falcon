"""The Run button: the compiled pipeline started and stopped from the designer, how far the run is
shown in the bar at the bottom, and what its program writes held by the designer."""
import os
import time
from pathlib import Path

from designer_driver import Designer

# A build takes seconds; this is for one that hangs.
SECONDS_FOR_A_BUILD = 150
# A pipeline starts and stops within a second; this is for one that hangs.
SECONDS_FOR_A_RUN_TO_CHANGE = 15


def run_state(designer):
    return designer.state()["run"]["state"]


def wait_until(designer, is_reached, what, seconds=SECONDS_FOR_A_RUN_TO_CHANGE):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        designer.frames(1)
        if is_reached():
            return
        time.sleep(0.05)
    raise AssertionError(f"never reached: {what}")


def compiled_generator_into_logger(designer):
    """An EventGenerator into an EventLogger on core 0, compiled: a pipeline that logs an event a
    second."""
    generator = designer.add_processor("EventGenerator", core=0)
    logger = designer.add_processor("EventLogger", core=0)
    designer.connect((generator, "output"), (logger, "inputs"))
    designer.click("top_bar/compile")
    wait_until(designer, lambda: designer.state()["build"]["state"] != "running",
               "the end of the build", SECONDS_FOR_A_BUILD)
    assert designer.state()["build"]["state"] == "succeeded"
    return generator, logger


def programs_running_from(home):
    """The programs that run from under this directory, e.g. a pipeline that was never stopped."""
    home = str(Path(home).resolve()) + os.sep
    found = []
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                program = os.readlink(entry / "exe")
            except OSError:
                continue
            if program.startswith(home):
                found.append(program)
    return found


def test_pipeline_that_is_not_compiled_is_not_run():
    with Designer() as designer:
        assert designer.state()["bar"] == "Not compiled yet"
        designer.add_processor("EventGenerator", core=0)
        designer.click("top_bar/run")
        designer.frames(3)
        assert run_state(designer) == "none"
        assert programs_running_from(designer.home) == []


def test_run_starts_the_pipeline_and_the_same_button_stops_it():
    with Designer() as designer:
        compiled_generator_into_logger(designer)
        assert designer.state()["bar"] == "Compiled"

        designer.click("top_bar/run")
        designer.frames(1)
        assert run_state(designer) in ("starting", "running")
        wait_until(designer, lambda: run_state(designer) == "running", "the pipeline running")
        assert designer.state()["bar"] == "Running"
        assert len(programs_running_from(designer.home)) == 1
        lines = designer.state()["run"]["lines"]
        assert any(line.endswith("log engine: all processors are built, the pipeline is running")
                   for line in lines)

        # The pipeline's own lines come while it runs: its logger writes the first event at once.
        wait_until(designer,
                   lambda: any(" log event_logger: " in line
                               for line in designer.state()["run"]["lines"]),
                   "a line of the event logger")

        # A compilation would replace the executable under the run, so it isn't started.
        designer.click("top_bar/compile")
        designer.frames(2)
        assert designer.state()["build"]["state"] == "succeeded"

        designer.click("top_bar/run")
        wait_until(designer, lambda: run_state(designer) == "none", "the end of the run")
        assert designer.state()["bar"] == "Compiled"
        assert programs_running_from(designer.home) == []
        assert designer.state()["run"]["lines"][-1].endswith("log engine: the run ended")


def test_change_to_the_graph_takes_the_run_button_until_the_next_compilation():
    with Designer() as designer:
        compiled_generator_into_logger(designer)
        designer.add_processor("Trigger", core=1)
        designer.frames(2)
        assert designer.state()["bar"] == "Not compiled yet"
        designer.click("top_bar/run")
        designer.frames(3)
        assert run_state(designer) == "none"
        assert programs_running_from(designer.home) == []


def test_pipeline_is_stopped_when_falcon_is_closed():
    with Designer() as designer:
        compiled_generator_into_logger(designer)
        designer.click("top_bar/run")
        wait_until(designer, lambda: run_state(designer) == "running", "the pipeline running")
        home = designer.home
        runs = home / "falcon" / "prototype_folder" / "runs"
        log_file = next(runs.iterdir()) / "falcon.log"
        # Closing Falcon is the driver's "quit", which the with statement sends. Looked at before
        # the driver's own net kills what is left: Falcon must have stopped its pipeline itself.
        designer.close_falcon()
        assert programs_running_from(home) == []
        assert log_file.read_text().rstrip().endswith("log engine: the run ended")


def test_log_panel_is_opened_and_closed_from_the_bar_and_resized_by_its_top_edge():
    with Designer() as designer:
        assert not designer.has_target("log_panel")
        designer.click("bottom_bar/log")
        assert designer.has_target("log_panel")
        height = designer.target("log_panel").height
        cores_bottom = designer.target("core/0").bottom

        edge = designer.target("log_panel_edge").point()
        designer.start_drag("log_panel_edge")
        designer.drag_to((edge[0], edge[1] - 120))
        designer.drop()
        assert abs(designer.target("log_panel").height - (height + 120)) < 3
        # It stays under the cores, which keep a height worth working in.
        edge = designer.target("log_panel_edge").point()
        designer.start_drag("log_panel_edge")
        designer.drag_to((edge[0], edge[1] - 2000))
        designer.drop()
        assert designer.target("log_panel").top > cores_bottom

        designer.click("log_panel/close")
        assert not designer.has_target("log_panel")
        designer.click("bottom_bar/log")
        designer.click("bottom_bar/log")
        assert not designer.has_target("log_panel")


def test_log_panel_comes_up_with_a_run_and_spans_the_window():
    with Designer() as designer:
        compiled_generator_into_logger(designer)
        assert not designer.has_target("log_panel")
        designer.click("top_bar/run")
        wait_until(designer, lambda: run_state(designer) == "running", "the pipeline running")
        panel = designer.target("log_panel")
        bar = designer.target("bottom_bar/state")
        assert panel.bottom < bar.top
        assert panel.left < designer.target("panel/Recorder").left
        assert panel.right > designer.target("top_bar/compile").left
        designer.click("top_bar/run")
        wait_until(designer, lambda: run_state(designer) == "none", "the end of the run")
        # What the run wrote stays to be read after it ended.
        assert designer.has_target("log_panel")
        assert designer.state()["run"]["lines"][-1].endswith("log engine: the run ended")
