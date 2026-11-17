"""The driver itself: what a test relies on when it plays the mouse and reads the answers."""
from designer_driver import Designer, DesignerError


def test_command_the_driver_does_not_know_is_refused_and_the_run_goes_on():
    with Designer() as designer:
        try:
            designer.command("dance")
        except DesignerError as error:
            assert "unknown command: dance" in str(error)
        else:
            raise AssertionError("an unknown command was answered as if it were known")
        assert designer.state()["processors"] == []


def test_name_that_nothing_has_is_reported_with_the_names_there_are():
    with Designer() as designer:
        try:
            designer.click("block/nobody")
        except DesignerError as error:
            assert "nothing is named block/nobody" in str(error) and "panel/Trigger" in str(error)
        else:
            raise AssertionError("a click on a name that nothing has went through")


def test_what_is_named_follows_what_is_drawn():
    with Designer() as designer:
        assert not designer.has_target("block/trigger")
        designer.add_processor("Trigger", core=0)
        for name in ("block/trigger", "block/trigger/handle", "block/trigger/remove",
                     "port/trigger/input/input", "port/trigger/output/output"):
            block = designer.target("block/trigger")
            part = designer.target(name)
            assert block.left <= part.left and part.right <= block.right, name
            assert block.top <= part.top and part.bottom <= block.bottom, name
        designer.click("block/trigger/remove")
        assert not designer.has_target("block/trigger")
