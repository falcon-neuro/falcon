"""Where a core's processors are drawn, which follows from the graph alone: its rows from the
top, and in each row the processors from the left, each with its column in blocks."""
from designer_driver import Designer


def test_chain_stands_in_one_column():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        trigger = designer.add_processor("Trigger", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.connect((detector, "output"), (trigger, "input"))
        designer.connect((trigger, "output"), (logger, "inputs"))
        assert designer.rows(0) == [[(detector, 0)], [(trigger, 0)], [(logger, 0)]]


def test_block_of_a_reader_is_drawn_under_the_one_it_reads():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.connect((generator, "output"), (logger, "inputs"))
        writer = designer.target(f"block/{generator}")
        reader = designer.target(f"block/{logger}")
        assert reader.top > writer.bottom
        assert reader.left == writer.left


def test_readers_of_one_output_stand_side_by_side_under_it():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        trigger = designer.add_processor("Trigger", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.connect((detector, "output"), (logger, "inputs"))
        designer.connect((detector, "output"), (trigger, "input"))
        # In the order of the core's list, and the one they read is centred over them.
        assert designer.rows(0) == [[(detector, 0.5)], [(trigger, 0), (logger, 1)]]


def test_reader_of_two_stands_in_the_middle_under_both():
    with Designer() as designer:
        first = designer.add_processor("EventGenerator", core=0)
        second = designer.add_processor("EventGenerator", core=0)
        recorder = designer.add_processor("Recorder", core=0)
        designer.connect((first, "output"), (recorder, "inputs"))
        designer.connect((second, "output"), (recorder, "inputs"))
        assert designer.rows(0) == [[(first, 0), (second, 1)], [(recorder, 0.5)]]


def test_reader_of_two_stays_under_the_first_where_the_middle_is_taken():
    with Designer() as designer:
        first = designer.add_processor("EventGenerator", core=0)
        second = designer.add_processor("EventGenerator", core=0)
        recorder = designer.add_processor("Recorder", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.connect((first, "output"), (recorder, "inputs"))
        designer.connect((second, "output"), (recorder, "inputs"))
        designer.connect((first, "output"), (logger, "inputs"))
        assert designer.rows(0) == [[(first, 0.5), (second, 2)], [(recorder, 0), (logger, 1)]]


def test_unconnected_processor_stands_in_the_top_row_beside_the_chains():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.connect((generator, "output"), (logger, "inputs"))
        trigger = designer.add_processor("Trigger", core=0)
        assert designer.rows(0) == [[(generator, 0), (trigger, 1)], [(logger, 0)]]


def test_processors_that_feed_each_other_stand_in_the_top_row():
    with Designer() as designer:
        first = designer.add_processor("IirFilter", core=0)
        second = designer.add_processor("IirFilter", core=0)
        designer.connect((first, "output"), (second, "input"))
        designer.connect((second, "output"), (first, "input"))
        assert designer.rows(0) == [[(first, 0), (second, 1)]]


def test_connection_of_a_state_value_places_nothing():
    with Designer() as designer:
        first = designer.add_processor("RippleDetector", core=0)
        second = designer.add_processor("RippleDetector", core=0)
        designer.connect((first, "is_in_lockout"), (second, "is_detection_enabled"))
        assert designer.connections() == [(f"{first}.is_in_lockout",
                                           f"{second}.is_detection_enabled")]
        assert designer.rows(0) == [[(first, 0), (second, 1)]]


def test_reader_of_another_core_stands_at_the_top_of_its_own():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        trigger = designer.add_processor("Trigger", core=0)
        designer.connect((detector, "output"), (trigger, "input"))
        logger = designer.add_processor("EventLogger", core=1)
        designer.connect((trigger, "output"), (logger, "inputs"))
        assert designer.rows(0) == [[(detector, 0)], [(trigger, 0)]]
        assert designer.rows(1) == [[(logger, 0)]]


def test_row_is_as_high_as_its_tallest_block():
    with Designer() as designer:
        generator = designer.add_processor("EventGenerator", core=0)
        # The block of a processor that starts a stream is taller by the line that shows it.
        signal_generator = designer.add_processor("SignalGenerator", core=0)
        logger = designer.add_processor("EventLogger", core=0)
        designer.connect((generator, "output"), (logger, "inputs"))
        targets = designer.targets()
        tall, short = targets[f"block/{signal_generator}"], targets[f"block/{generator}"]
        assert tall.height > short.height and tall.top == short.top
        assert targets[f"block/{logger}"].top > tall.bottom
