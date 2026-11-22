"""The Compile button: the graph written as pipeline.cpp and built beside the UI, how that went
shown in the bar at the bottom, and the pipeline loaded again when Falcon starts."""
import tempfile
import time

from designer_driver import Designer

# A build takes seconds; this is for one that hangs. Less than a run of Falcon may last.
SECONDS_FOR_A_BUILD = 150


def prototype_folder(designer):
    return designer.home / "falcon" / "prototype_folder"


def build_state(designer):
    return designer.state()["build"]


def wait_for_the_build(designer):
    end = time.monotonic() + SECONDS_FOR_A_BUILD
    while time.monotonic() < end:
        designer.frames(1)
        if build_state(designer)["state"] != "running":
            return build_state(designer)
        time.sleep(0.1)
    raise AssertionError("the build didn't end")


def generator_into_logger(designer):
    generator = designer.add_processor("EventGenerator", core=0)
    logger = designer.add_processor("EventLogger", core=0)
    designer.connect((generator, "output"), (logger, "inputs"))
    return generator, logger


def test_nothing_is_compiled_at_first():
    with Designer() as designer:
        assert build_state(designer) == {"state": "none", "text": "", "problems": [],
                                         "is_panel_open": False, "is_raw_shown": False}
        assert designer.has_target("top_bar/compile")


def test_warning_in_the_bar_opens_and_closes_the_problems_panel():
    with Designer() as designer:
        assert not designer.has_target("problems_panel")
        designer.click("bottom_bar/warning")
        assert build_state(designer)["is_panel_open"] and designer.has_target("problems_panel")
        designer.click("bottom_bar/warning")
        assert not build_state(designer)["is_panel_open"]
        assert not designer.has_target("problems_panel")


def test_compile_builds_the_pipeline_into_its_folder_while_the_window_stays_usable():
    with Designer() as designer:
        generator_into_logger(designer)
        designer.click("top_bar/compile")
        assert build_state(designer)["state"] == "running"
        trigger = designer.add_processor("Trigger", core=1)
        assert trigger in designer.processors()

        assert wait_for_the_build(designer)["state"] == "succeeded"
        folder = prototype_folder(designer)
        # What was compiled is the graph as it stood at the click; the file holds it as it is now.
        compiled = (folder / ".last_compiled_pipeline.cpp").read_text()
        assert "EventGenerator<" in compiled and "Trigger<" not in compiled
        designer.frames(3)
        time.sleep(0.2)
        assert "Trigger<" in (folder / "pipeline.cpp").read_text()
        assert (folder / "pipeline").stat().st_size > 0


def test_pipeline_is_there_again_at_the_next_start():
    with tempfile.TemporaryDirectory() as home:
        with Designer(home=home) as designer:
            generator, logger = generator_into_logger(designer)
            designer.click("top_bar/compile")
            assert wait_for_the_build(designer)["state"] == "succeeded"
            connections = designer.connections()
        with Designer(home=home) as designer:
            assert set(designer.processors()) == {generator, logger}
            assert designer.connections() == connections


def test_graph_that_cannot_be_written_fails_and_its_problem_comes_up_by_itself():
    with Designer() as designer:
        first = designer.add_processor("RippleDetector", core=0)
        second = designer.add_processor("RippleDetector", core=0)
        designer.connect((first, "is_in_lockout"), (second, "is_detection_enabled"))
        designer.click("top_bar/compile")
        state = build_state(designer)
        assert state["state"] == "failed"
        assert len(state["problems"]) == 1
        problem = state["problems"][0]
        assert problem["title"] == "State values can't be compiled yet"
        assert "the writer doesn't write a connection of a state value" in problem["body"]
        assert problem["processor"] == second
        assert state["is_panel_open"] and designer.has_target("problems_panel/problem/0")

        # The panel is closed by its cross or by the warning, which also opens it again.
        designer.click("problems_panel/close")
        assert not build_state(designer)["is_panel_open"]
        designer.click("bottom_bar/warning")
        assert build_state(designer)["is_panel_open"]
        designer.click("bottom_bar/warning")
        assert not build_state(designer)["is_panel_open"]

        # A compilation that fails again brings the panel up again.
        designer.click("top_bar/compile")
        assert build_state(designer)["is_panel_open"]


def test_problem_of_the_designer_has_a_title_a_body_and_its_processor():
    with Designer() as designer:
        generator = designer.add_processor("SignalGenerator", core=0)
        designer.click("top_bar/compile")
        assert build_state(designer)["problems"] == [{
            "title": "An output isn't connected",
            "body": "its output output isn't connected to anything, and a SignalGenerator needs "
                    "it connected",
            "place": "",
            "processor": generator,
        }]


def test_panel_at_the_right_is_made_wider_and_narrower_by_its_left_edge():
    with Designer() as designer:
        designer.click("bottom_bar/warning")
        width = designer.target("problems_panel").width
        edge = designer.target("right_panel_edge").point()

        designer.start_drag("right_panel_edge")
        designer.drag_to((edge[0] - 150, edge[1]))
        designer.drop()
        wider = designer.target("problems_panel").width
        assert abs(wider - (width + 150)) < 3

        # A processor's options show in the same panel, as wide as it was left.
        trigger = designer.add_processor("Trigger", core=0)
        designer.click(f"block/{trigger}/handle")
        assert abs(designer.target("options_panel").width - wider) < 3

        # It never gets narrower than it can be read.
        edge = designer.target("right_panel_edge").point()
        designer.start_drag("right_panel_edge")
        designer.drag_to((edge[0] + 900, edge[1]))
        designer.drop()
        assert abs(designer.target("options_panel").width - 240) < 3


def test_problems_and_a_processor_s_options_take_each_other_s_place():
    with Designer() as designer:
        recorder = designer.add_processor("Recorder", core=0)
        designer.click(f"block/{recorder}/handle")
        assert designer.has_target("options_panel")

        designer.click("bottom_bar/warning")
        assert designer.has_target("problems_panel") and not designer.has_target("options_panel")
        assert designer.state()["selected"] is None

        designer.click(f"block/{recorder}/handle")
        assert designer.has_target("options_panel") and not designer.has_target("problems_panel")
        assert not build_state(designer)["is_panel_open"]


def test_panel_goes_when_a_compilation_succeeds():
    with Designer() as designer:
        recorder = designer.add_processor("Recorder", core=0)
        designer.click("top_bar/compile")
        assert wait_for_the_build(designer)["state"] == "failed"
        designer.frames(2)
        assert build_state(designer)["is_panel_open"]

        generator = designer.add_processor("SignalGenerator", core=0)
        designer.connect((generator, "output"), (recorder, "inputs"))
        designer.click("top_bar/compile")
        assert wait_for_the_build(designer)["state"] == "succeeded"
        designer.frames(2)
        assert not build_state(designer)["is_panel_open"]


def test_error_of_the_compiler_is_a_problem_in_the_processor_s_own_words():
    with Designer() as designer:
        recorder = designer.add_processor("Recorder", core=0)
        designer.click("top_bar/compile")
        state = wait_for_the_build(designer)
        assert state["state"] == "failed"
        assert not (prototype_folder(designer) / "pipeline").exists()
        problem = next(problem for problem in state["problems"]
                       if problem["title"] == "a recorder needs at least one connection")
        assert problem["place"].startswith("recorder.cpp, line ")
        # The error lies in the Recorder's own file, and is this recorder's all the same: the
        # compiler names the line of pipeline.cpp that it is required from.
        assert problem["processor"] == recorder

        # The list is shown first; what the compiler wrote is one click away, and back.
        designer.frames(2)
        assert not build_state(designer)["is_raw_shown"]
        designer.click("problems_panel/raw")
        assert build_state(designer)["is_raw_shown"]
        assert not designer.has_target("problems_panel/problem/0")
        designer.click("problems_panel/raw")
        assert designer.has_target("problems_panel/problem/0")


def test_error_on_a_line_of_pipeline_cpp_is_a_problem_of_that_line_s_processor():
    with Designer() as designer:
        generator = designer.add_processor("SignalGenerator", core=0)
        recorder = designer.add_processor("Recorder", core=1)
        designer.connect((generator, "output"), (recorder, "inputs"))
        # A file's name typed without its quotes is no text for the compiler.
        designer.click(f"block/{recorder}/handle")
        designer.click("options_panel/input/file_name", across=0.25)
        designer.key("Ctrl+A")
        designer.type_text("recording.bin")
        designer.key("Enter")
        designer.click("top_bar/compile")
        state = wait_for_the_build(designer)
        assert state["state"] == "failed"
        problem = next(problem for problem in state["problems"]
                       if "was not declared in this scope" in problem["title"])
        assert problem["place"].startswith("pipeline.cpp, line ")
        assert problem["processor"] == recorder
        assert all(problem["processor"] in (recorder, "") for problem in state["problems"])
        assert all(problem["processor"] != generator for problem in state["problems"])
