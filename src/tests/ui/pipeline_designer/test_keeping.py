"""Every action is kept: the graph is written as pipeline.cpp as soon as it changes, also while
it isn't finished, and is there again when Falcon starts."""
import tempfile
import time
from pathlib import Path

from designer_driver import Designer


def pipeline_file(home):
    return Path(home) / "falcon" / "prototype_folder" / "pipeline.cpp"


def kept_text_with(designer, part):
    """The file's text once it holds this part: the file is written beside the UI's thread."""
    end = time.monotonic() + 5
    while time.monotonic() < end:
        file = pipeline_file(designer.home)
        if file.exists() and part in file.read_text():
            return file.read_text()
        time.sleep(0.02)
    raise AssertionError(f"pipeline.cpp never held {part}")


def test_nothing_is_written_before_the_first_action():
    with Designer() as designer:
        assert not pipeline_file(designer.home).exists()
        assert designer.state()["keeping_note"] == ""


def test_action_is_kept_at_once_while_the_graph_is_not_finished():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        kept_text_with(designer, f"SpikeDetector<InputStreamInfo, {{.threshold = -50}}> {detector}")

        generator = designer.add_processor("SignalGenerator", core=0)
        designer.connect((generator, "output"), (detector, "input"))
        kept_text_with(designer,
                       f"SpikeDetector<SignalGeneratorStream, {{.threshold = -50}}> {detector}")
        assert designer.state()["keeping_note"] == ""


def test_unfinished_pipeline_is_there_again_at_the_next_start():
    with tempfile.TemporaryDirectory() as home:
        with Designer(home=home) as designer:
            generator = designer.add_processor("SignalGenerator", core=0)
            detector = designer.add_processor("SpikeDetector", core=1)
            logger = designer.add_processor("EventLogger", core=1)
            designer.connect((detector, "output"), (logger, "inputs"))
            connections = designer.connections()
        with Designer(home=home) as designer:
            assert set(designer.processors()) == {generator, detector, logger}
            assert designer.connections() == connections
            assert designer.core_list(0) == [generator]


def test_removing_the_last_processor_is_kept_too():
    with tempfile.TemporaryDirectory() as home:
        with Designer(home=home) as designer:
            trigger = designer.add_processor("Trigger", core=0)
            designer.click(f"block/{trigger}/remove")
            assert designer.processors() == {}
        with Designer(home=home) as designer:
            assert designer.processors() == {}


def test_designer_is_not_reached_with_a_file_that_cannot_be_read():
    with tempfile.TemporaryDirectory() as home:
        file = pipeline_file(home)
        file.parent.mkdir(parents=True)
        file.write_text("this is no pipeline\n")
        with Designer(home=home) as designer:
            assert designer.state()["screen"] == "explorer"
            designer.frames(5)
            assert file.read_text() == "this is no pipeline\n"
