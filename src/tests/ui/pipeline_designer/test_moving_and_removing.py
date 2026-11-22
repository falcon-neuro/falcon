"""Moving a processor that exists, and removing one."""
from designer_driver import Designer, port


def chain_and_lone_trigger(designer):
    """On core 0: an EventGenerator with an EventLogger under it, and a Trigger beside them."""
    generator = designer.add_processor("EventGenerator", core=0)
    logger = designer.add_processor("EventLogger", core=0)
    designer.connect((generator, "output"), (logger, "inputs"))
    trigger = designer.add_processor("Trigger", core=0)
    assert designer.rows(0) == [[(generator, 0), (trigger, 1)], [(logger, 0)]]
    return generator, logger, trigger


def test_block_dragged_into_another_core_moves_that_processor_alone():
    with Designer() as designer:
        generator, logger, trigger = chain_and_lone_trigger(designer)
        designer.drag_to_drop_place(f"block/{logger}/handle", core=1)
        designer.drop()

        processors = designer.processors()
        assert processors[logger]["core"] == 1
        assert processors[generator]["core"] == 0 and processors[trigger]["core"] == 0
        # Its connections stay as they are.
        assert designer.connections() == [(f"{generator}.output", f"{logger}.inputs")]
        assert designer.core_list(1) == [logger]


def test_block_dragged_into_another_core_lands_at_the_drop_place():
    with Designer() as designer:
        first = designer.add_processor("Trigger", core=0)
        on_core_1 = designer.add_processor("Trigger", core=1)
        designer.drag_to_drop_place(f"block/{first}/handle", core=1, drop_place=0)
        designer.drop()
        assert designer.core_list(1) == [first, on_core_1]


def test_block_dragged_and_released_outside_every_core_stays_where_it_was():
    with Designer() as designer:
        chain_and_lone_trigger(designer)
        before = designer.state()
        designer.start_drag("block/event_logger/handle")
        designer.drag_to(designer.target("panel/Recorder").point())
        designer.drop()
        assert designer.state() == before


def test_block_dragged_inside_its_core_moves_its_whole_chain():
    with Designer() as designer:
        generator, logger, trigger = chain_and_lone_trigger(designer)
        # By the logger, which is not the top of its chain, to the right of the trigger.
        designer.drag_to_drop_place(f"block/{logger}/handle", core=0)
        designer.drop()

        assert designer.rows(0) == [[(trigger, 0), (generator, 1)], [(logger, 1)]]
        assert designer.connections() == [(f"{generator}.output", f"{logger}.inputs")]
        assert all(processor["core"] == 0 for processor in designer.processors().values())


def test_chain_dropped_beside_itself_stays_where_it_is():
    with Designer() as designer:
        generator, logger, trigger = chain_and_lone_trigger(designer)
        before = designer.rows(0)
        for drop_place in (0, 1):
            designer.drag_to_drop_place(f"block/{generator}/handle", core=0,
                                        drop_place=drop_place)
            designer.drop()
            assert designer.rows(0) == before


def test_remove_icon_removes_the_processor_and_its_connections():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        trigger = designer.add_processor("Trigger", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.connect((detector, "output"), (trigger, "input"))
        designer.connect((trigger, "output"), (logger, "inputs"))

        designer.click(f"block/{trigger}/remove")
        assert sorted(designer.processors()) == sorted([detector, logger])
        assert designer.connections() == []
        assert designer.core_list(0) == [detector, logger]


def test_remove_icon_cannot_be_clicked_while_a_port_is_picked():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.click(port(generator, "output", "output"))
        designer.click(f"block/{logger}/remove")
        assert sorted(designer.processors()) == [generator, logger]
        assert designer.state()["picked"] is None
