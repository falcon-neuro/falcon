"""A processor's options: in short on its block, and edited in the panel at the right."""
from designer_driver import Designer, port


def option_of(designer, processor, option):
    return designer.processors()[processor]["options"][option]


def type_into_input(designer, option, text):
    """Types a new value into an option's input in the panel, in place of the one it holds, and
    leaves the input being typed in."""
    # Left of the middle: a number's box has its two step buttons at its right end.
    designer.click(f"options_panel/input/{option}", across=0.25)
    designer.key("Ctrl+A")
    designer.type_text(text)


def replace_input(designer, option, text):
    type_into_input(designer, option, text)
    designer.key("Enter")


def test_block_has_a_line_for_each_of_its_options():
    with Designer() as designer:
        detector = designer.add_processor("LevelCrossingDetector", core=0)
        block = designer.target(f"block/{detector}")
        lines = [designer.target(f"block/{detector}/option/{option}")
                 for option in ("threshold", "direction", "post_detection_block")]
        assert all(block.top < line.top and line.bottom < block.bottom for line in lines)
        assert lines[0].bottom <= lines[1].top and lines[1].bottom <= lines[2].top


def test_option_line_stays_inside_its_block():
    with Designer() as designer:
        generator = designer.add_processor("SignalGenerator", core=0)
        targets = designer.targets()
        block = targets[f"block/{generator}"]
        lines = [target for name, target in targets.items()
                 if name.startswith(f"block/{generator}/option/")]
        assert len(lines) == 10
        too_wide = [line.name for line in lines if line.right > block.right]
        assert too_wide == []


def test_click_on_a_blocks_name_opens_its_options_and_escape_closes_them():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        assert designer.state()["selected"] is None
        assert not designer.has_target("options_panel")

        designer.click(f"block/{detector}/handle")
        assert designer.state()["selected"] == detector
        assert designer.has_target("options_panel")
        assert designer.has_target("options_panel/input/threshold")

        designer.key("Escape")
        assert designer.state()["selected"] is None
        assert not designer.has_target("options_panel")


def test_click_on_an_option_line_opens_the_options_and_the_cross_closes_them():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/option/threshold")
        assert designer.state()["selected"] == detector
        designer.click("options_panel/close")
        assert designer.state()["selected"] is None
        assert not designer.has_target("options_panel")


def test_click_on_another_block_shows_that_ones_options():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        output = designer.add_processor("PCIeParallelOutput", core=0)
        designer.click(f"block/{detector}/handle")
        designer.click(f"block/{output}/handle")
        assert designer.state()["selected"] == output
        assert designer.has_target("options_panel/input/pin")
        assert not designer.has_target("options_panel/input/threshold")


def test_removing_the_selected_processor_closes_its_options():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/handle")
        designer.click(f"block/{detector}/remove")
        assert designer.state()["selected"] is None
        assert not designer.has_target("options_panel")


def test_escape_lets_a_picked_port_go_before_it_closes_the_options():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/handle")
        designer.click(port(detector, "output", "output"))
        designer.key("Escape")
        assert designer.state()["picked"] is None
        assert designer.state()["selected"] == detector
        designer.key("Escape")
        assert designer.state()["selected"] is None


def test_text_option_holds_what_is_typed():
    with Designer() as designer:
        benchmark = designer.add_processor("LatencyBenchmark", core=0)
        assert option_of(designer, benchmark, "file_name") == '"latency_benchmark.csv"'
        designer.click(f"block/{benchmark}/handle")
        replace_input(designer, "file_name", '"first run.csv"')
        assert option_of(designer, benchmark, "file_name") == '"first run.csv"'


def test_number_option_holds_the_number_typed():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        assert option_of(designer, detector, "threshold") == "-50"
        designer.click(f"block/{detector}/handle")
        replace_input(designer, "threshold", "-42.5")
        assert option_of(designer, detector, "threshold") == "-42.5"


def test_text_option_follows_each_character_typed():
    with Designer() as designer:
        benchmark = designer.add_processor("LatencyBenchmark", core=0)
        designer.click(f"block/{benchmark}/handle")
        type_into_input(designer, "file_name", '"a')
        assert option_of(designer, benchmark, "file_name") == '"a'
        designer.type_text('.csv"')
        assert option_of(designer, benchmark, "file_name") == '"a.csv"'


def test_number_typed_is_kept_when_the_box_is_left():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/handle")
        type_into_input(designer, "threshold", "-33")
        designer.click("core/0", down=0.9)
        assert option_of(designer, detector, "threshold") == "-33"
        assert designer.state()["selected"] == detector


def test_escape_in_an_input_takes_back_what_was_typed_and_keeps_the_options_open():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/handle")
        type_into_input(designer, "threshold", "-33")
        designer.key("Escape")
        assert option_of(designer, detector, "threshold") == "-50"
        assert designer.state()["selected"] == detector


def test_step_buttons_of_a_number_box_change_the_number_by_one():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/handle")
        # The two buttons are at the right end of the input, minus left of plus.
        designer.click("options_panel/input/threshold", across=0.985)
        assert option_of(designer, detector, "threshold") == "-49"
        designer.click("options_panel/input/threshold", across=0.985)
        assert option_of(designer, detector, "threshold") == "-48"


def test_number_box_takes_no_letters():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/handle")
        replace_input(designer, "threshold", "12x4")
        assert option_of(designer, detector, "threshold") == "124"


def test_number_box_takes_no_calculation():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/handle")
        replace_input(designer, "threshold", "1000/30")
        # Either the slash can't be typed, or the value stays what it was.
        assert option_of(designer, detector, "threshold") in ("100030", "-50")


def test_whole_number_option_takes_no_decimals():
    with Designer() as designer:
        output = designer.add_processor("PCIeParallelOutput", core=0)
        assert option_of(designer, output, "pin") == "5"
        designer.click(f"block/{output}/handle")
        replace_input(designer, "pin", "7")
        assert option_of(designer, output, "pin") == "7"
        replace_input(designer, "pin", "3.9")
        assert option_of(designer, output, "pin").isdigit()


def test_button_puts_the_default_back_and_shows_only_for_a_changed_value():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/handle")
        assert not designer.has_target("options_panel/default/threshold")

        replace_input(designer, "threshold", "-30")
        assert designer.has_target("options_panel/default/threshold")
        designer.click("options_panel/default/threshold")
        assert option_of(designer, detector, "threshold") == "-50"
        assert not designer.has_target("options_panel/default/threshold")


def test_number_written_differently_still_counts_as_the_default():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/handle")
        replace_input(designer, "threshold", "-50.0")
        assert not designer.has_target("options_panel/default/threshold")
