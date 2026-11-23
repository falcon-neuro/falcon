"""A processor's options: shown and edited in the panel at the right, and nowhere on its block."""
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


def test_block_shows_none_of_its_options_which_the_panel_alone_shows():
    with Designer() as designer:
        detector = designer.add_processor("LevelCrossingDetector", core=0)
        generator = designer.add_processor("SignalGenerator", core=0)
        assert not any("/option/" in name for name in designer.targets())
        # A block is as tall with ten options as with three: what it holds is its name and type.
        assert (designer.target(f"block/{detector}/handle").height ==
                designer.target(f"block/{generator}/handle").height)
        designer.click(f"block/{detector}/handle")
        for option in ("threshold", "direction", "post_detection_block"):
            assert designer.has_target(f"options_panel/input/{option}")
        assert not any("/option/" in name for name in designer.targets())


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


def test_cross_of_the_panel_closes_the_options():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/handle")
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


def test_option_with_a_fixed_list_of_values_is_chosen_from_a_list():
    with Designer() as designer:
        generator = designer.add_processor("SignalGenerator", core=0)
        assert option_of(designer, generator, "waveform") == "Waveform::Sine"
        designer.click(f"block/{generator}/handle")
        designer.click("options_panel/input/waveform")
        designer.click("options_panel/choice/waveform/Square")
        assert option_of(designer, generator, "waveform") == "Waveform::Square"

        assert designer.has_target("options_panel/default/waveform")
        designer.click("options_panel/default/waveform")
        assert option_of(designer, generator, "waveform") == "Waveform::Sine"


def test_list_of_values_offers_every_value_and_only_while_it_is_open():
    with Designer() as designer:
        detector = designer.add_processor("LevelCrossingDetector", core=0)
        designer.click(f"block/{detector}/handle")
        assert not designer.has_target("options_panel/choice/direction/Upward")
        designer.click("options_panel/input/direction")
        assert designer.has_target("options_panel/choice/direction/Upward")
        assert designer.has_target("options_panel/choice/direction/Downward")
        designer.click("options_panel/choice/direction/Downward")
        assert not designer.has_target("options_panel/choice/direction/Upward")
        assert option_of(designer, detector, "direction") == "CrossingDirection::Downward"


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
