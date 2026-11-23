"""A block at rest holds its type and name alone. It shows its ports, as rows inside it, while the
mouse is over it, while a connection is drawn, and while the button above the cores shows every
block's."""
from designer_driver import PLACE_OVER_NO_BLOCK, Designer, port


def processors_showing_ports(designer):
    """The processors whose ports the last frame named."""
    return sorted({name.split("/")[1] for name in designer.targets() if name.startswith("port/")})


def take_mouse_off_the_blocks(designer):
    designer.move_to(PLACE_OVER_NO_BLOCK)
    designer.frames(4)


def bring_mouse_over(designer, processor):
    designer.move_to(designer.target(f"block/{processor}/handle").point())
    designer.frames(4)


def test_block_at_rest_shows_no_port():
    with Designer() as designer:
        designer.add_processor("Trigger", core=0)
        take_mouse_off_the_blocks(designer)
        assert processors_showing_ports(designer) == []


def test_block_under_the_mouse_shows_its_ports_and_hides_them_when_the_mouse_leaves():
    with Designer() as designer:
        trigger = designer.add_processor("Trigger", core=0)
        take_mouse_off_the_blocks(designer)
        height_at_rest = designer.target(f"block/{trigger}").height

        bring_mouse_over(designer, trigger)
        assert processors_showing_ports(designer) == [trigger]
        block = designer.target(f"block/{trigger}")
        assert block.height > height_at_rest
        for direction, name in (("input", "input"), ("output", "output")):
            row = designer.targets()[port(trigger, direction, name)]
            assert block.top <= row.top and row.bottom <= block.bottom

        take_mouse_off_the_blocks(designer)
        assert processors_showing_ports(designer) == []
        assert designer.target(f"block/{trigger}").height == height_at_rest


def test_only_the_block_under_the_mouse_shows_its_ports():
    with Designer() as designer:
        first = designer.add_processor("Trigger", core=0)
        second = designer.add_processor("Trigger", core=0)
        bring_mouse_over(designer, first)
        assert processors_showing_ports(designer) == [first]
        take_mouse_off_the_blocks(designer)
        bring_mouse_over(designer, second)
        assert processors_showing_ports(designer) == [second]


def test_block_that_shows_its_ports_moves_the_block_under_it_and_keeps_the_gap():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.connect((generator, "output"), (logger, "inputs"))
        take_mouse_off_the_blocks(designer)
        upper = designer.target(f"block/{generator}")
        lower = designer.target(f"block/{logger}")
        gap_at_rest = lower.top - upper.bottom
        assert gap_at_rest > 0

        bring_mouse_over(designer, generator)
        upper_open = designer.target(f"block/{generator}")
        lower_moved = designer.target(f"block/{logger}")
        assert upper_open.top == upper.top and upper_open.bottom > upper.bottom
        assert lower_moved.top > lower.top
        assert lower_moved.top - upper_open.bottom == gap_at_rest


def test_no_block_shows_its_ports_while_a_processor_is_dragged():
    with Designer() as designer:
        trigger = designer.add_processor("Trigger", core=0)
        take_mouse_off_the_blocks(designer)
        designer.start_drag("panel/EventLogger")
        designer.drag_to(designer.target(f"block/{trigger}").point())
        assert processors_showing_ports(designer) == []
        designer.drag_to(designer.target("panel/Recorder").point())
        designer.drop()


def test_button_above_the_cores_shows_the_ports_of_every_block_and_hides_them_again():
    with Designer() as designer:
        first = designer.add_processor("Trigger", core=0)
        second = designer.add_processor("EventLogger", core=1)
        designer.click("ports_toggle")
        take_mouse_off_the_blocks(designer)
        assert processors_showing_ports(designer) == sorted([first, second])
        designer.click("ports_toggle")
        take_mouse_off_the_blocks(designer)
        assert processors_showing_ports(designer) == []


def test_every_block_shows_its_ports_while_a_connection_is_drawn():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=1)
        designer.click(port(generator, "output", "output"))
        take_mouse_off_the_blocks(designer)
        assert designer.state()["picked"] is not None
        assert processors_showing_ports(designer) == sorted([generator, logger])

        designer.key("Escape")
        take_mouse_off_the_blocks(designer)
        assert designer.state()["picked"] is None
        assert processors_showing_ports(designer) == []


def test_inputs_stand_with_the_shortest_name_on_top_and_outputs_with_it_at_the_bottom():
    # A port's line runs from where its name ends straight to the block's edge, past the rows
    # between, so those rows hold the shorter names.
    with Designer() as designer:
        detector = designer.add_processor("RippleDetector", core=0)
        bring_mouse_over(designer, detector)
        targets = designer.targets()

        def rows_from_the_top(direction):
            start = f"port/{detector}/{direction}/"
            rows = [(target.top, name[len(start):]) for name, target in targets.items()
                    if name.startswith(start)]
            return [name for _, name in sorted(rows)]

        inputs = rows_from_the_top("input")
        assert sorted(inputs) == ["input", "is_detection_enabled", "threshold_multiplier"]
        assert inputs[0] == "input"
        assert rows_from_the_top("output") == ["is_in_lockout", "output"]


def test_click_on_a_block_s_name_selects_it_though_its_ports_come_in_above_the_name():
    with Designer() as designer:
        detector = designer.add_processor("RippleDetector", core=0)
        take_mouse_off_the_blocks(designer)
        name_at_rest = designer.target(f"block/{detector}/handle")
        designer.click(f"block/{detector}/handle")
        assert designer.target(f"block/{detector}/handle").top > name_at_rest.top
        assert designer.state()["selected"] == detector
        assert designer.state()["picked"] is None
