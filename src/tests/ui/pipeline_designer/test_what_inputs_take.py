"""What every processor's inputs take: samples, events or a state value. The table below is
written by hand, from what each processor is for. Every input in the panel must stand in it, so a
new processor's input is checked against what its author means, never against what the list of
processors happens to say."""
from designer_driver import Designer, port

SAMPLES, EVENTS, STATE_VALUE = "samples", "events", "a state value"

TAKEN_BY_INPUT = {
    ("EventLogger", "inputs"): {EVENTS},
    ("FirFilter", "input"): {SAMPLES},
    ("IirFilter", "input"): {SAMPLES},
    ("LatencyBenchmark", "input"): {EVENTS},
    ("LevelCrossingDetector", "input"): {SAMPLES},
    ("PCIeParallelOutput", "input"): {EVENTS},
    ("Recorder", "inputs"): {SAMPLES, EVENTS},
    ("RippleDetector", "input"): {SAMPLES},
    ("RippleDetector", "threshold_multiplier"): {STATE_VALUE},
    ("RippleDetector", "is_detection_enabled"): {STATE_VALUE},
    ("SpikeDetector", "input"): {SAMPLES},
    ("Trigger", "input"): {EVENTS},
}


def note_of(designer, name):
    """Why a port can't be clicked while another is picked; empty for one that can."""
    return designer.target(name).note


def inputs_of(designer, processor):
    return designer.port_names(processor, "input")


def kinds_taken_by_inputs_of(type_name):
    """What each input of a processor of this type takes, as the designer judges it: an output of
    each kind is picked in turn, and an input takes that kind unless it says that it takes
    another."""
    with Designer() as designer:
        sources = {
            SAMPLES: (designer.add_processor("SignalGenerator", core=0), "output"),
            EVENTS: (designer.add_processor("EventGenerator", core=1), "output"),
            STATE_VALUE: (designer.add_processor("RippleDetector", core=2), "is_in_lockout"),
        }
        processor = designer.add_processor(type_name, core=3)
        taken = {input_name: set() for input_name in inputs_of(designer, processor)}
        for kind, (source, output_name) in sources.items():
            designer.click(port(source, "output", output_name))
            for input_name in taken:
                note = note_of(designer, port(processor, "input", input_name))
                # An input that takes events of other kinds than the picked one's still takes
                # events: it names the kinds, where one of another kind says "gives events."
                if f"and the picked output gives {kind}." not in note:
                    taken[input_name].add(kind)
            designer.key("Escape")
        return taken


def test_every_input_takes_what_its_processor_is_for_and_nothing_else():
    with Designer() as designer:
        type_names = sorted(name[len("panel/"):] for name in designer.targets()
                            if name.startswith("panel/"))
    taken_by_input = {}
    for type_name in type_names:
        for input_name, kinds in kinds_taken_by_inputs_of(type_name).items():
            taken_by_input[(type_name, input_name)] = kinds
    assert taken_by_input == TAKEN_BY_INPUT


def test_samples_can_not_be_connected_to_an_input_with_slots_that_takes_events_only():
    with Designer() as designer:
        generator = designer.add_processor("SignalGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.click(port(generator, "output", "output"))
        assert (note_of(designer, port(logger, "input", "inputs")) ==
                "This input takes events, and the picked output gives samples.")
        designer.click(port(logger, "input", "inputs"))
        assert designer.connections() == []


def test_input_with_slots_that_takes_samples_and_events_is_connected_to_both():
    with Designer() as designer:
        signal_generator = designer.add_processor("SignalGenerator", core=0)
        event_generator = designer.add_processor("EventGenerator", core=0)
        recorder = designer.add_processor("Recorder", core=1)
        designer.connect((signal_generator, "output"), (recorder, "inputs"))
        designer.connect((event_generator, "output"), (recorder, "inputs"))
        assert len(designer.connections()) == 2
