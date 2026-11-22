"""An output's name: its own at first, and the user's once one is typed in the panel."""
from designer_driver import Designer


def name_of_output(designer, processor):
    return designer.processors()[processor]["output_names"]["output"]


def type_output_name(designer, text):
    designer.click("options_panel/output_name/output")
    designer.key("Ctrl+A")
    designer.type_text(text)
    designer.key("Enter")


def test_output_is_called_after_its_processor_and_itself_at_first():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        assert name_of_output(designer, detector) == f"{detector}_output"
        designer.click(f"block/{detector}/handle")
        assert designer.has_target("options_panel/output_name/output")
        assert not designer.has_target("options_panel/own_output_name/output")


def test_name_typed_in_the_panel_becomes_the_name_of_the_output():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/handle")
        type_output_name(designer, "spikes")
        assert name_of_output(designer, detector) == "spikes"


def test_button_puts_the_own_name_of_the_output_back():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/handle")
        type_output_name(designer, "spikes")
        designer.click("options_panel/own_output_name/output")
        assert name_of_output(designer, detector) == f"{detector}_output"
        assert not designer.has_target("options_panel/own_output_name/output")


def test_name_takes_only_the_characters_of_a_name():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/handle")
        type_output_name(designer, "my spikes, 2nd!")
        assert name_of_output(designer, detector) == "myspikes2nd"


def test_text_that_starts_with_a_digit_leaves_the_name_as_it_was():
    with Designer() as designer:
        detector = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{detector}/handle")
        type_output_name(designer, "9lives")
        assert name_of_output(designer, detector) == f"{detector}_output"


def test_processor_without_an_output_has_no_name_to_change():
    with Designer() as designer:
        logger = designer.add_processor("EventLogger", core=0)
        assert designer.processors()[logger]["output_names"] == {}
        designer.click(f"block/{logger}/handle")
        assert not designer.has_target("options_panel/output_name/output")


def test_name_that_another_output_has_is_marked():
    with Designer() as designer:
        first = designer.add_processor("SpikeDetector", core=0)
        second = designer.add_processor("SpikeDetector", core=0)
        designer.click(f"block/{second}/handle")
        assert not designer.has_target("options_panel/output_name_shared/output")
        type_output_name(designer, f"{first}_output")
        assert name_of_output(designer, second) == f"{first}_output"
        assert designer.has_target("options_panel/output_name_shared/output")
        type_output_name(designer, "other_spikes")
        assert not designer.has_target("options_panel/output_name_shared/output")
