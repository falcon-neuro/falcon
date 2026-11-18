"""Which kinds of event an input takes: an input is greyed for an output whose events it doesn't
take, and says which kinds it does take."""
from designer_driver import Designer, port

# What Trigger says of its input in its source, which a greyed port shows under its reason.
TRIGGER_INPUT = ("Events that each name the channels something happened on, e.g. spikes or level "
                 "crossings.")


def note_of(designer, name):
    """Why a port can't be clicked while another is picked; empty for one that can."""
    return designer.target(name).note


def test_input_is_greyed_for_a_kind_of_event_it_does_not_take():
    with Designer() as designer:
        ripple_detector = designer.add_processor("RippleDetector", core=0)
        trigger = designer.add_processor("Trigger", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.click(port(ripple_detector, "output", "output"))

        assert note_of(designer, port(trigger, "input", "input")) == (
            "This input takes LevelCrossings or Spikes events, and the picked output gives Ripple "
            "events.\n\n" + TRIGGER_INPUT)
        assert note_of(designer, port(logger, "input", "input")) == ""


def test_click_on_an_input_that_does_not_take_the_event_connects_nothing():
    with Designer() as designer:
        ripple_detector = designer.add_processor("RippleDetector", core=0)
        trigger = designer.add_processor("Trigger", core=0)
        designer.click(port(ripple_detector, "output", "output"))
        designer.click(port(trigger, "input", "input"))
        assert designer.state()["picked"] is None
        assert designer.connections() == []


def test_output_is_greyed_for_a_picked_input_that_does_not_take_its_events():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        spike_detector = designer.add_processor("SpikeDetector", core=0)
        trigger = designer.add_processor("Trigger", core=0)
        designer.click(port(trigger, "input", "input"))

        assert note_of(designer, port(generator, "output", "output")) == (
            "This output gives ScheduledEvent events, and the picked input takes LevelCrossings or "
            "Spikes events.\n\nThe picked input: " + TRIGGER_INPUT)
        assert note_of(designer, port(spike_detector, "output", "output")) == ""


def test_input_takes_each_kind_it_is_listed_with():
    with Designer() as designer:
        spike_detector = designer.add_processor("SpikeDetector", core=0)
        crossing_detector = designer.add_processor("LevelCrossingDetector", core=0)
        first_trigger = designer.add_processor("Trigger", core=0)
        second_trigger = designer.add_processor("Trigger", core=0)
        designer.connect((spike_detector, "output"), (first_trigger, "input"))
        designer.connect((crossing_detector, "output"), (second_trigger, "input"))
        assert len(designer.connections()) == 2


def test_event_of_no_sample_is_not_taken_where_a_sample_is_needed():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        benchmark = designer.add_processor("LatencyBenchmark", core=0)
        designer.click(port(generator, "output", "output"))

        note = note_of(designer, port(benchmark, "input", "input"))
        assert note.startswith(
            "This input takes LevelCrossings, Ripple, Spikes or TriggerEvent events, and the "
            "picked output gives ScheduledEvent events.\n\n")


def test_port_with_slots_takes_every_kind_of_event():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        ripple_detector = designer.add_processor("RippleDetector", core=0)
        spike_detector = designer.add_processor("SpikeDetector", core=0)
        recorder = designer.add_processor("Recorder", core=0)
        designer.connect((generator, "output"), (recorder, "inputs"))
        designer.connect((ripple_detector, "output"), (recorder, "inputs"))
        designer.connect((spike_detector, "output"), (recorder, "inputs"))
        assert len(designer.connections()) == 3
