"""Bringing a processor into existence, and which cores have a box."""
from pathlib import Path

from designer_driver import Designer


def test_panel_lists_inputs_then_operators_then_outputs():
    with Designer() as designer:
        targets = designer.targets()
        inputs = ["EventGenerator", "SignalGenerator"]
        operators = ["FirFilter", "IirFilter", "LevelCrossingDetector", "RippleDetector",
                     "SpikeDetector", "Trigger"]
        outputs = ["EventLogger", "LatencyBenchmark", "PCIeParallelOutput", "Recorder"]
        lowest_input = max(targets[f"panel/{name}"].bottom for name in inputs)
        highest_operator = min(targets[f"panel/{name}"].top for name in operators)
        lowest_operator = max(targets[f"panel/{name}"].bottom for name in operators)
        highest_output = min(targets[f"panel/{name}"].top for name in outputs)
        assert lowest_input < highest_operator
        assert lowest_operator < highest_output


def test_empty_pipeline_has_the_box_of_the_first_core_offered():
    with Designer() as designer:
        state = designer.state()
        first_offered = state["offered_cores"][0]
        assert state["shown_cores"] == [first_offered]
        assert designer.has_target(f"core/{first_offered}")
        assert state["processors"] == []


def cores_listed_in(path):
    """The cores a file of the kernel lists, e.g. "0-3,8" as [0, 1, 2, 3, 8]."""
    cores = []
    for part in Path(path).read_text().strip().split(","):
        if part:
            first, _, last = part.partition("-")
            cores.extend(range(int(first), int(last or first) + 1))
    return cores


def test_isolated_cores_are_offered_first_then_the_others_lowest_number_first():
    with Designer() as designer:
        isolated = cores_listed_in("/sys/devices/system/cpu/isolated")
        present = cores_listed_in("/sys/devices/system/cpu/present")
        others = [core for core in present if core not in isolated]
        assert designer.state()["offered_cores"] == sorted(isolated) + sorted(others)


def test_processor_dragged_from_the_panel_sits_unconnected_in_its_core():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        processors = designer.processors()
        assert (generator, logger) == ("event_generator", "event_logger")
        assert processors[generator]["type"] == "EventGenerator"
        assert processors[generator]["core"] == 0 and processors[logger]["core"] == 0
        # A drop never makes a connection, however well the two would fit.
        assert designer.connections() == []
        assert designer.rows(0) == [[(generator, 0), (logger, 1)]]


def test_a_second_processor_of_a_type_gets_a_name_of_its_own():
    with Designer() as designer:
        designer.add_processor("Trigger", core=0)
        assert designer.add_processor("Trigger", core=0) == "trigger_2"


def test_drag_released_outside_every_core_adds_nothing():
    with Designer() as designer:
        designer.start_drag("panel/Trigger")
        designer.drag_to(designer.target("panel/Recorder").point())
        designer.drop()
        assert designer.state()["processors"] == []


def test_every_core_offered_shows_during_a_drag_and_stays_only_if_used():
    with Designer() as designer:
        designer.add_processor("Trigger", core=0)
        assert designer.state()["shown_cores"] == [0]
        assert not designer.has_target("core/1")

        designer.start_drag("panel/Trigger")
        state = designer.state()
        assert state["shown_cores"] == sorted(state["offered_cores"])
        assert designer.has_target("core/1")
        designer.drag_to(designer.target("panel/Recorder").point())
        designer.drop()
        assert designer.state()["shown_cores"] == [0]
        assert not designer.has_target("core/1")

        second = designer.add_processor("Trigger", core=1)
        assert designer.processors()[second]["core"] == 1
        assert designer.state()["shown_cores"] == [0, 1]
        assert designer.has_target("core/1")


def test_only_the_cores_that_hold_a_processor_have_a_box():
    with Designer() as designer:
        designer.add_processor("Trigger", core=0)
        on_core_1 = designer.add_processor("Trigger", core=1)
        on_core_2 = designer.add_processor("Trigger", core=2)
        assert designer.state()["shown_cores"] == [0, 1, 2]

        # A core between two in use that holds no processor any more has no box.
        designer.click(f"block/{on_core_1}/remove")
        assert designer.state()["shown_cores"] == [0, 2]
        assert not designer.has_target("core/1")
        assert designer.target("core/0").right < designer.target("core/2").left

        designer.click(f"block/{on_core_2}/remove")
        assert designer.state()["shown_cores"] == [0]


def test_drop_place_decides_the_place_among_the_chains():
    with Designer() as designer:
        designer.add_processor("Trigger", core=0)
        designer.add_processor("SpikeDetector", core=0, drop_place=0)
        assert designer.core_list(0) == ["spike_detector", "trigger"]
        designer.add_processor("EventLogger", core=0)
        assert designer.core_list(0) == ["spike_detector", "trigger", "event_logger"]
        designer.add_processor("EventGenerator", core=0, drop_place=1)
        assert designer.core_list(0) == ["spike_detector", "event_generator", "trigger",
                                         "event_logger"]
        assert designer.rows(0) == [[("spike_detector", 0), ("event_generator", 1),
                                     ("trigger", 2), ("event_logger", 3)]]


def test_a_drop_place_stands_left_of_each_chain_and_right_of_the_last():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.connect((generator, "output"), (logger, "inputs"))
        trigger = designer.add_processor("Trigger", core=0)

        # Two chains: the generator with its logger under it, and the trigger.
        designer.drag_over_core("panel/Recorder", core=0)
        places = designer.drop_places()
        targets = designer.targets()
        assert len(places) == 3
        centres = [place.point()[0] for place in places]
        assert centres[0] < targets[f"block/{generator}"].left
        assert targets[f"block/{generator}"].right < centres[1] < targets[f"block/{trigger}"].left
        assert targets[f"block/{trigger}"].right < centres[2]
        designer.drop()


def test_drop_place_nearest_the_mouse_is_the_marked_one():
    with Designer() as designer:
        designer.add_processor("Trigger", core=0)
        designer.add_processor("Trigger", core=0)

        designer.drag_over_core("panel/Trigger", core=0, across=0.02)
        assert [place.note for place in designer.drop_places()] == ["marked", "", ""]
        designer.drag_to(designer.target("core/0").point(across=0.98))
        assert [place.note for place in designer.drop_places()] == ["", "", "marked"]

        # A drop anywhere in the box goes to the marked one.
        designer.drop()
        assert designer.core_list(0) == ["trigger", "trigger_2", "trigger_3"]


def test_only_the_core_under_the_mouse_shows_drop_places():
    with Designer() as designer:
        designer.add_processor("Trigger", core=0)
        designer.add_processor("Trigger", core=0)
        designer.add_processor("Trigger", core=1)

        designer.start_drag("panel/EventLogger")
        assert designer.drop_places() == []

        core_0 = designer.target("core/0")
        designer.drag_to(core_0.point())
        places = designer.drop_places()
        assert len(places) == 3
        assert all(core_0.left <= place.left and place.right <= core_0.right for place in places)

        core_1 = designer.target("core/1")
        designer.drag_to(core_1.point())
        places = designer.drop_places()
        assert len(places) == 2
        assert all(core_1.left <= place.left and place.right <= core_1.right for place in places)

        designer.drag_to(designer.target("panel/Recorder").point())
        assert designer.drop_places() == []
        designer.drop()


def test_core_without_processors_shows_one_place_to_drop_at():
    with Designer() as designer:
        designer.drag_over_core("panel/Trigger", core=0)
        places = designer.drop_places()
        assert len(places) == 1 and places[0].note == "marked"
        designer.drop()
        assert designer.core_list(0) == ["trigger"]


def test_nothing_moves_while_a_processor_is_dragged():
    with Designer() as designer:
        designer.add_processor("Trigger", core=0)
        designer.add_processor("Trigger", core=0)
        designer.add_processor("Trigger", core=1)

        def places_of_boxes_and_blocks():
            return {name: target for name, target in designer.targets().items()
                    if name in ("core/0", "core/1") or name.startswith("block/")}

        # When a drag begins, the block under the mouse hides its ports and every core offered
        # gets a box. From then on nothing moves, wherever the mouse goes.
        designer.start_drag("panel/EventLogger")
        before = places_of_boxes_and_blocks()
        designer.drag_to(designer.target("core/0").point())
        assert places_of_boxes_and_blocks() == before
        designer.drag_to(designer.target("core/1").point())
        assert places_of_boxes_and_blocks() == before
        designer.drag_to(designer.target("panel/Recorder").point())
        designer.drop()
        assert places_of_boxes_and_blocks() == before
