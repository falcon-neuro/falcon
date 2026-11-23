"""A connection between two cores is a hop. The designer gives it a ring, shown over the box of the
core that reads it with its number of slots, and a slider on the ring sets that number. A state
value hops without a ring."""
import tempfile

from designer_driver import LEFT_BUTTON, PLACE_OVER_NO_BLOCK, Designer
from test_keeping import kept_text_with


def rings(designer):
    """Each ring the last frame named, with its number of slots."""
    return {name: int(target.note) for name, target in designer.targets().items()
            if name.startswith("ring/")}


def ring_name(processor, output, reading_core):
    return f"ring/{processor}/{output}/{reading_core}"


def move_slider_of_ring(designer, ring, across):
    """Opens the ring's box, drags its slider to this part of its width, and closes the box."""
    designer.click(ring)
    slider = designer.target("ring_slots")
    designer.move_to(slider.point(across=0.45))
    designer.frames(1)
    designer.command(f"mouse_down {LEFT_BUTTON}")
    designer.frames(1)
    designer.move_to(slider.point(across=across))
    designer.frames(2)
    designer.command(f"mouse_up {LEFT_BUTTON}")
    designer.frames(2)
    designer.click_at(PLACE_OVER_NO_BLOCK)
    assert not designer.has_target("ring_slots")


def generator_and_logger_on_two_cores(designer):
    generator = designer.add_processor("EventGenerator", core=0)
    logger = designer.add_processor("EventLogger", core=1)
    designer.connect((generator, "output"), (logger, "inputs"))
    designer.key("Escape")
    return generator, logger


def test_connection_inside_a_core_has_no_ring():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.connect((generator, "output"), (logger, "inputs"))
        assert rings(designer) == {}


def test_connection_between_two_cores_gets_a_ring_of_64_slots():
    with Designer() as designer:
        generator, _ = generator_and_logger_on_two_cores(designer)
        assert rings(designer) == {ring_name(generator, "output", 1): 64}


def test_ring_stands_above_the_box_of_the_core_that_reads_it():
    with Designer() as designer:
        generator, _ = generator_and_logger_on_two_cores(designer)
        ring = designer.target(ring_name(generator, "output", 1))
        reading_box = designer.target("core/1")
        writing_box = designer.target("core/0")
        assert ring.bottom <= reading_box.top
        assert writing_box.right < ring.right <= reading_box.right


def test_connections_of_one_output_into_one_core_share_a_ring():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=1)
        recorder = designer.add_processor("Recorder", core=1)
        designer.connect((generator, "output"), (logger, "inputs"))
        designer.connect((generator, "output"), (recorder, "inputs"))
        designer.key("Escape")
        assert rings(designer) == {ring_name(generator, "output", 1): 64}


def test_output_read_on_two_cores_has_a_ring_for_each():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=1)
        recorder = designer.add_processor("Recorder", core=2)
        designer.connect((generator, "output"), (logger, "inputs"))
        designer.connect((generator, "output"), (recorder, "inputs"))
        designer.key("Escape")
        assert rings(designer) == {ring_name(generator, "output", 1): 64,
                                   ring_name(generator, "output", 2): 64}


def test_state_value_crosses_cores_without_a_ring():
    with Designer() as designer:
        first = designer.add_processor("RippleDetector", core=0)
        second = designer.add_processor("RippleDetector", core=1)
        designer.connect((first, "is_in_lockout"), (second, "is_detection_enabled"))
        designer.key("Escape")
        assert designer.connections() == [(f"{first}.is_in_lockout",
                                           f"{second}.is_detection_enabled")]
        assert rings(designer) == {}


def test_slider_on_a_ring_sets_a_power_of_two_from_1_to_16384():
    with Designer() as designer:
        generator, _ = generator_and_logger_on_two_cores(designer)
        ring = ring_name(generator, "output", 1)
        for across, slots in ((0.0, 1), (1.0, 16384), (0.5, 128)):
            move_slider_of_ring(designer, ring, across)
            assert rings(designer) == {ring: slots}


def test_number_of_slots_set_on_a_ring_is_in_the_file_and_there_again_at_the_next_start():
    with tempfile.TemporaryDirectory() as home:
        with Designer(home=home) as designer:
            generator, _ = generator_and_logger_on_two_cores(designer)
            ring = ring_name(generator, "output", 1)
            kept_text_with(designer, ", 64>")
            move_slider_of_ring(designer, ring, 1.0)
            kept_text_with(designer, ", 16384>")
        with Designer(home=home) as designer:
            assert rings(designer) == {ring: 16384}


def test_ring_goes_when_its_last_connection_does():
    with Designer() as designer:
        generator, logger = generator_and_logger_on_two_cores(designer)
        designer.right_click(f"port/{logger}/input/inputs")
        assert designer.connections() == []
        assert rings(designer) == {}


def test_box_of_a_core_is_wider_by_the_lanes_of_its_hops_and_wider_for_a_larger_ring():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=1)
        designer.move_to(PLACE_OVER_NO_BLOCK)
        designer.frames(4)
        widths_without_hop = designer.target("core/0").width, designer.target("core/1").width

        designer.connect((generator, "output"), (logger, "inputs"))
        designer.key("Escape")
        designer.move_to(PLACE_OVER_NO_BLOCK)
        designer.frames(4)
        widths_with_hop = designer.target("core/0").width, designer.target("core/1").width
        assert widths_with_hop[0] > widths_without_hop[0]
        assert widths_with_hop[1] > widths_without_hop[1]

        # The hop's line between the boxes is the wider the more slots its ring has, and its
        # lanes in the two boxes make room for it.
        move_slider_of_ring(designer, ring_name(generator, "output", 1), 1.0)
        designer.frames(4)
        assert designer.target("core/0").width > widths_with_hop[0]
        assert designer.target("core/1").width > widths_with_hop[1]
