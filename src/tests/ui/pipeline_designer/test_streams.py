"""A stream: set on the processor that starts it, and taken by those after it."""
from designer_driver import Designer


def stream_of(designer, processor):
    return designer.processors()[processor]["stream"]


def stream_taken_by(designer, processor):
    return designer.processors()[processor]["takes_stream"]


def replace_stream_value(designer, value_name, text):
    # Left of the middle: the box has its two step buttons at its right end.
    designer.click(f"options_panel/stream/{value_name}", across=0.25)
    designer.key("Ctrl+A")
    designer.type_text(text)
    designer.key("Enter")


def test_processor_that_starts_a_stream_has_one_and_shows_it():
    with Designer() as designer:
        generator = designer.add_processor("SignalGenerator", core=0)
        assert stream_of(designer, generator) == {"channel_count": 32, "rate": 30000}
        block = designer.target(f"block/{generator}")
        line = designer.target(f"block/{generator}/stream")
        assert block.top < line.top and line.bottom < block.bottom

        designer.click(f"block/{generator}/stream")
        assert designer.state()["selected"] == generator
        assert designer.has_target("options_panel/stream/channel_count")
        assert designer.has_target("options_panel/stream/rate")


def test_processor_that_starts_no_stream_has_none():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        assert stream_of(designer, detector) is None
        assert not designer.has_target(f"block/{detector}/stream")
        designer.click(f"block/{detector}/handle")
        assert not designer.has_target("options_panel/stream/channel_count")
        assert not designer.has_target("options_panel/stream/rate")


def test_channel_count_and_rate_are_set_in_the_panel():
    with Designer() as designer:
        generator = designer.add_processor("SignalGenerator", core=0)
        designer.click(f"block/{generator}/handle")
        replace_stream_value(designer, "channel_count", "384")
        replace_stream_value(designer, "rate", "24414.0625")
        assert stream_of(designer, generator) == {"channel_count": 384, "rate": 24414.0625}


def test_channel_count_stays_one_at_least_and_rate_above_zero():
    with Designer() as designer:
        generator = designer.add_processor("SignalGenerator", core=0)
        designer.click(f"block/{generator}/handle")
        replace_stream_value(designer, "channel_count", "0")
        replace_stream_value(designer, "rate", "0")
        assert stream_of(designer, generator) == {"channel_count": 32, "rate": 30000}
        replace_stream_value(designer, "channel_count", "1")
        assert stream_of(designer, generator)["channel_count"] == 1
        designer.click("options_panel/stream/channel_count/minus")
        assert stream_of(designer, generator)["channel_count"] == 1


def test_stream_is_taken_by_the_processors_after_the_one_that_starts_it():
    with Designer() as designer:
        generator = designer.add_processor("SignalGenerator", core=0)
        filtered = designer.add_processor("IirFilter", core=0)
        detector = designer.add_processor("SpikeDetector", core=0)
        assert stream_taken_by(designer, filtered) is None
        assert stream_taken_by(designer, detector) is None

        designer.connect((filtered, "output"), (detector, "input"))
        assert stream_taken_by(designer, detector) is None

        designer.connect((generator, "output"), (filtered, "input"))
        assert stream_taken_by(designer, filtered) == {"channel_count": 32, "rate": 30000}
        assert stream_taken_by(designer, detector) == {"channel_count": 32, "rate": 30000}

        designer.click(f"block/{generator}/handle")
        replace_stream_value(designer, "channel_count", "64")
        assert stream_taken_by(designer, detector) == {"channel_count": 64, "rate": 30000}


def test_processor_takes_no_stream_from_what_starts_none():
    with Designer() as designer:
        generator = designer.add_processor("SignalGenerator", core=0)
        assert stream_taken_by(designer, generator) is None
