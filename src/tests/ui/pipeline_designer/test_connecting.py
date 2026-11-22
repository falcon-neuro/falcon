"""Connecting two ports with two clicks, and taking a connection away."""
from designer_driver import Designer, port


def generator_and_logger(designer):
    """An EventGenerator and an EventLogger on core 0, whose output and input fit."""
    return (designer.add_processor("EventGenerator", core=0),
            designer.add_processor("EventLogger", core=0))


def test_click_on_an_output_then_on_an_input_connects_them():
    with Designer() as designer:
        generator, logger = generator_and_logger(designer)
        designer.click(port(generator, "output", "output"))
        assert designer.state()["picked"] == {"direction": "output", "processor": generator,
                                              "port": "output"}
        assert designer.connections() == []
        designer.click(port(logger, "input", "inputs"))
        assert designer.connections() == [(f"{generator}.output", f"{logger}.inputs")]
        assert designer.state()["picked"] is None


def test_click_on_an_input_then_on_an_output_connects_them():
    with Designer() as designer:
        generator, logger = generator_and_logger(designer)
        designer.click(port(logger, "input", "inputs"))
        assert designer.state()["picked"] == {"direction": "input", "processor": logger,
                                              "port": "inputs"}
        designer.click(port(generator, "output", "output"))
        assert designer.connections() == [(f"{generator}.output", f"{logger}.inputs")]
        assert designer.state()["picked"] is None


def test_connection_is_made_between_two_cores_too():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=1)
        designer.connect((generator, "output"), (logger, "inputs"))
        assert designer.connections() == [(f"{generator}.output", f"{logger}.inputs")]


def test_one_output_feeds_any_number_of_inputs():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        first = designer.add_processor("EventLogger", core=0)
        second = designer.add_processor("EventLogger", core=0)
        designer.connect((generator, "output"), (first, "inputs"))
        designer.connect((generator, "output"), (second, "inputs"))
        assert designer.connections() == [(f"{generator}.output", f"{first}.inputs"),
                                          (f"{generator}.output", f"{second}.inputs")]


def test_an_input_with_slots_takes_several_connections():
    with Designer() as designer:
        first = designer.add_processor("EventGenerator", core=0)
        second = designer.add_processor("EventGenerator", core=0)
        recorder = designer.add_processor("Recorder", core=0)
        designer.connect((first, "output"), (recorder, "inputs"))
        designer.connect((second, "output"), (recorder, "inputs"))
        assert designer.connections() == [(f"{first}.output", f"{recorder}.inputs"),
                                          (f"{second}.output", f"{recorder}.inputs")]


def picked_port_is_let_go_by(let_go):
    with Designer() as designer:
        generator, logger = generator_and_logger(designer)
        designer.click(port(generator, "output", "output"))
        assert designer.state()["picked"] is not None
        let_go(designer, generator, logger)
        assert designer.state()["picked"] is None
        assert designer.connections() == []


def test_second_click_on_the_picked_port_lets_it_go():
    picked_port_is_let_go_by(
        lambda designer, generator, logger: designer.click(port(generator, "output", "output")))


def test_escape_lets_the_picked_port_go():
    picked_port_is_let_go_by(lambda designer, generator, logger: designer.key("Escape"))


def test_right_click_anywhere_lets_the_picked_port_go():
    picked_port_is_let_go_by(
        lambda designer, generator, logger: designer.right_click("panel/Recorder"))


def test_right_click_on_a_port_that_fits_lets_the_picked_port_go_too():
    picked_port_is_let_go_by(
        lambda designer, generator, logger: designer.right_click(port(logger, "input", "inputs")))


def test_click_on_an_empty_place_lets_the_picked_port_go():
    picked_port_is_let_go_by(
        lambda designer, generator, logger: designer.click("core/0", down=0.9))


def test_click_on_a_port_that_does_not_fit_lets_the_picked_port_go():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(port(generator, "output", "output"))
        # The detector's input takes samples, the generator gives events.
        designer.click(port(detector, "input", "input"))
        assert designer.state()["picked"] is None
        assert designer.connections() == []


def note_of(designer, name):
    """Why a port can't be clicked while another is picked; empty for one that can."""
    return designer.target(name).note


def test_while_an_output_is_picked_only_the_inputs_it_fits_can_be_clicked():
    with Designer() as designer:
        crossing_detector = designer.add_processor("LevelCrossingDetector", core=0)
        trigger = designer.add_processor("Trigger", core=0)
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(port(crossing_detector, "output", "output"))

        assert note_of(designer, port(crossing_detector, "output", "output")) == ""
        assert note_of(designer, port(trigger, "input", "input")) == ""
        assert (note_of(designer, port(detector, "input", "input")) ==
                "This input takes samples, and the picked output gives events.")
        other_output = "An output connects to an input, not to another output."
        assert note_of(designer, port(trigger, "output", "output")) == other_output
        assert note_of(designer, port(detector, "output", "output")) == other_output


def test_while_an_input_is_picked_only_the_outputs_that_fit_it_can_be_clicked():
    with Designer() as designer:
        spike_detector = designer.add_processor("SpikeDetector", core=0)
        signal = designer.add_processor("LevelCrossingDetector", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        trigger = designer.add_processor("Trigger", core=0)
        designer.click(port(trigger, "input", "input"))

        assert note_of(designer, port(trigger, "input", "input")) == ""
        assert note_of(designer, port(spike_detector, "output", "output")) == ""
        assert note_of(designer, port(signal, "output", "output")) == ""
        other_input = "An input connects to an output, not to another input."
        assert note_of(designer, port(logger, "input", "inputs")) == other_input
        assert note_of(designer, port(signal, "input", "input")) == other_input
        assert (note_of(designer, port(trigger, "output", "output")) ==
                "A processor's output can't be connected to its own input.")


def test_a_processor_cannot_be_connected_to_itself():
    with Designer() as designer:
        trigger = designer.add_processor("Trigger", core=0)
        designer.click(port(trigger, "output", "output"))
        assert (note_of(designer, port(trigger, "input", "input")) ==
                "A processor's output can't be connected to its own input.")
        designer.click(port(trigger, "input", "input"))
        assert designer.connections() == []


def test_an_input_takes_one_connection_only():
    with Designer() as designer:
        first = designer.add_processor("SpikeDetector", core=0)
        second = designer.add_processor("SpikeDetector", core=0)
        trigger = designer.add_processor("Trigger", core=0)
        designer.connect((first, "output"), (trigger, "input"))

        designer.click(port(second, "output", "output"))
        assert (note_of(designer, port(trigger, "input", "input")) ==
                "This input takes one connection and already has one.")
        designer.click(port(trigger, "input", "input"))
        assert designer.connections() == [(f"{first}.output", f"{trigger}.input")]

        # Picked itself, an input that has its connection fits no output.
        designer.click(port(trigger, "input", "input"))
        assert (note_of(designer, port(second, "output", "output")) ==
                "The picked input takes one connection and already has one.")
        assert (note_of(designer, port(first, "output", "output")) ==
                "This output is already connected to the picked input.")


def test_two_ports_are_connected_once_only():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        recorder = designer.add_processor("Recorder", core=0)
        designer.connect((generator, "output"), (recorder, "inputs"))
        designer.click(port(generator, "output", "output"))
        assert (note_of(designer, port(recorder, "input", "inputs")) ==
                "This input is already connected to the picked output.")
        designer.click(port(recorder, "input", "inputs"))
        assert designer.connections() == [(f"{generator}.output", f"{recorder}.inputs")]


def test_right_click_on_an_input_removes_its_connections():
    with Designer() as designer:
        first = designer.add_processor("EventGenerator", core=0)
        second = designer.add_processor("EventGenerator", core=0)
        recorder = designer.add_processor("Recorder", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.connect((first, "output"), (recorder, "inputs"))
        designer.connect((second, "output"), (recorder, "inputs"))
        designer.connect((first, "output"), (logger, "inputs"))

        designer.right_click(port(recorder, "input", "inputs"))
        assert designer.connections() == [(f"{first}.output", f"{logger}.inputs")]


def test_right_click_on_an_input_while_a_port_is_picked_only_lets_that_go():
    with Designer() as designer:
        first = designer.add_processor("EventGenerator", core=0)
        second = designer.add_processor("EventGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.connect((first, "output"), (logger, "inputs"))

        designer.click(port(second, "output", "output"))
        designer.right_click(port(logger, "input", "inputs"))
        assert designer.state()["picked"] is None
        assert designer.connections() == [(f"{first}.output", f"{logger}.inputs")]
