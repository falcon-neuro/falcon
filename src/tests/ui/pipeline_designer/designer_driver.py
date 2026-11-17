"""Plays the mouse and the keys in Falcon's pipeline designer, in a window that isn't shown.

Falcon is started with FALCON_UI_DRIVER set (see src/ui/ui_driver/ui_driver.cpp), takes one
command a line on its standard input and answers each with one line of JSON. A test names what it
clicks on ("port/trigger/input/input", "core/1", "panel/Trigger"): the designer tells where it
drew each named thing in the last frame, and hands over the graph it holds.

    with Designer() as designer:
        designer.add_processor("Trigger", core=0)
        assert designer.state()["processors"][0]["name"] == "trigger"
"""
import json
import os
import queue
import subprocess
import sys
import tempfile
import threading
from dataclasses import dataclass
from pathlib import Path

UI_TESTS_DIRECTORY = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(UI_TESTS_DIRECTORY))
from screenshot import read_ppm, write_png  # noqa: E402  (after the path it is found on)

REPOSITORY = UI_TESTS_DIRECTORY.parent.parent.parent
OUTPUT_DIRECTORY = REPOSITORY / "build" / "output"
ANSWER_MARKER = "FALCON_UI_DRIVER "
# A command is answered within milliseconds; this is for a Falcon that hangs.
SECONDS_FOR_AN_ANSWER = 20
# No test runs this long; a Falcon left behind by a test that died is stopped after it.
SECONDS_FOR_A_RUN = 180

LEFT_BUTTON = 0
RIGHT_BUTTON = 1

# Where a picture of the window is written when a test fails inside a with Designer(), so that
# what the test saw can be looked at; run.py sets it for each test. None writes no picture.
PICTURE_ON_FAILURE = None


class DesignerError(Exception):
    """Falcon didn't do what the driver asked: no answer, a refused command, a missing name."""


@dataclass(frozen=True)
class Target:
    """Something the designer named in the last frame, and where it drew it."""
    name: str
    left: float
    top: float
    right: float
    bottom: float
    note: str

    def point(self, across=0.5, down=0.5):
        """A point in it: across from its left edge and down from its top, each from 0 to 1."""
        return (self.left + (self.right - self.left) * across,
                self.top + (self.bottom - self.top) * down)

    @property
    def width(self):
        return self.right - self.left

    @property
    def height(self):
        return self.bottom - self.top


def port(processor, direction, name):
    """The name of a port's row: direction is "input" or "output"."""
    return f"port/{processor}/{direction}/{name}"


class Designer:
    """One run of Falcon with the driver on. Use it in a with statement."""

    def __init__(self):
        self._process = None
        self._answers = queue.Queue()
        self._stop_timer = None
        self._mouse = (0.0, 0.0)

    def __enter__(self):
        falcon = OUTPUT_DIRECTORY / "falcon"
        if not falcon.exists():
            raise DesignerError(f"{falcon} is missing: build Falcon first (sh scripts/make.sh)")
        environment = dict(os.environ, FALCON_UI_DRIVER="1")
        environment.pop("FALCON_SCREENSHOT", None)
        environment.pop("FALCON_EXAMPLE_PIPELINE", None)
        self._process = subprocess.Popen(
            [str(falcon)], cwd=OUTPUT_DIRECTORY, env=environment, text=True,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        threading.Thread(target=self._read_answers, daemon=True).start()
        self._stop_timer = threading.Timer(SECONDS_FOR_A_RUN, self._process.kill)
        self._stop_timer.daemon = True
        self._stop_timer.start()
        # What sizes itself to what it holds has its size only after a few frames.
        self.frames(8)
        return self

    def __exit__(self, exception_type, exception, traceback):
        self._stop_timer.cancel()
        if exception_type is not None and PICTURE_ON_FAILURE is not None:
            try:
                self.picture(PICTURE_ON_FAILURE)
            except (DesignerError, OSError, ValueError):
                pass  # the failure being reported is the test's, not the picture's
        try:
            self._process.stdin.write("quit\n")
            self._process.stdin.flush()
            self._process.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            # Only ever the Falcon this object started.
            self._process.kill()
            self._process.wait()
        return False

    def _read_answers(self):
        for line in self._process.stdout:
            if line.startswith(ANSWER_MARKER):
                self._answers.put(line[len(ANSWER_MARKER):])
        self._answers.put(None)

    # ------------------------------------------------------------------ commands

    def command(self, text):
        """Sends one command and returns its answer."""
        try:
            self._process.stdin.write(text + "\n")
            self._process.stdin.flush()
        except OSError as error:
            raise DesignerError(f"Falcon is gone: {error}") from error
        try:
            line = self._answers.get(timeout=SECONDS_FOR_AN_ANSWER)
        except queue.Empty:
            raise DesignerError(f"no answer to: {text}") from None
        if line is None:
            raise DesignerError(f"Falcon quit instead of answering: {text}")
        answer = json.loads(line)
        if not answer.get("ok"):
            raise DesignerError(f"{text}: {answer.get('error')}")
        return answer

    def frames(self, count=3):
        self.command(f"frames {count}")

    def targets(self):
        """Everything the last frame named, by name."""
        targets = {}
        for entry in self.command("targets")["targets"]:
            left, top, right, bottom = entry["rectangle"]
            targets[entry["name"]] = Target(entry["name"], left, top, right, bottom, entry["note"])
        return targets

    def target(self, name):
        targets = self.targets()
        if name not in targets:
            raise DesignerError(f"nothing is named {name}; named are: {sorted(targets)}")
        return targets[name]

    def has_target(self, name):
        return name in self.targets()

    def state(self):
        """What the designer holds: its graph, the picked port, the selected processor, ..."""
        return self.command("state")["state"]

    def picture(self, png_path):
        """Writes a picture of the window as it is now."""
        with tempfile.TemporaryDirectory() as directory:
            ppm_path = Path(directory) / "falcon.ppm"
            self.command(f"picture {ppm_path}")
            width, height, pixels = read_ppm(ppm_path)
        write_png(Path(png_path), width, height, pixels)

    # ------------------------------------------------------------------ mouse and keys

    def move_to(self, point):
        self._mouse = point
        self.command(f"mouse_move {point[0]:.1f} {point[1]:.1f}")

    def move_in_steps_to(self, point, steps=6):
        """As a hand moves the mouse: through the places in between, a frame at each."""
        start = self._mouse
        for step in range(1, steps + 1):
            self.move_to((start[0] + (point[0] - start[0]) * step / steps,
                          start[1] + (point[1] - start[1]) * step / steps))

    def click_at(self, point, button=LEFT_BUTTON):
        self.move_to(point)
        self.frames(1)
        self.command(f"mouse_down {button}")
        self.command(f"mouse_up {button}")
        self.frames(3)

    def click(self, name, button=LEFT_BUTTON, across=0.5, down=0.5):
        self.click_at(self.target(name).point(across, down), button)

    def right_click(self, name):
        self.click(name, RIGHT_BUTTON)

    def key(self, name):
        """A key or keys held together, e.g. "Escape" or "Ctrl+A"."""
        self.command(f"key {name}")
        self.frames(2)

    def type_text(self, text):
        self.command(f"text {text}")
        self.frames(1)

    def start_drag(self, name, across=0.5, down=0.5):
        """Presses on the named thing and moves far enough for a drag to begin."""
        point = self.target(name).point(across, down)
        self.move_to(point)
        self.frames(1)
        self.command(f"mouse_down {LEFT_BUTTON}")
        self.move_in_steps_to((point[0] + 12, point[1] + 12), steps=3)
        self.frames(2)

    def drag_to(self, point):
        self.move_in_steps_to(point)
        self.frames(3)

    def drop(self):
        self.command(f"mouse_up {LEFT_BUTTON}")
        self.frames(3)

    # ------------------------------------------------------------------ the designer

    def processors(self):
        """The processors of the graph, by name."""
        return {processor["name"]: processor for processor in self.state()["processors"]}

    def core_list(self, core):
        """The names in a core's list, in its order, which is the order its chains run in."""
        for entry in self.state()["cores"]:
            if entry["number"] == core:
                return entry["processors"]
        return []

    def rows(self, core):
        """A core's rows from the top: each a list of (name, column), from the left."""
        for entry in self.state()["cores"]:
            if entry["number"] == core:
                return [[(placed["processor"], placed["column"]) for placed in row]
                        for row in entry["rows"]]
        return []

    def connections(self):
        """Each connection as ("processor.port" of the output, "processor.port" of the input)."""
        return sorted((f"{c['output']['processor']}.{c['output']['port']}",
                       f"{c['input']['processor']}.{c['input']['port']}")
                      for c in self.state()["connections"])

    def drop_places(self):
        """The drop places shown now, from left to right; none unless a drag is over a core."""
        targets = self.targets()
        places = []
        while f"drop/{len(places)}" in targets:
            places.append(targets[f"drop/{len(places)}"])
        return places

    def drag_over_core(self, source, core, across=0.5, down=0.5):
        """Drags the named thing over a core's box and holds it there."""
        self.start_drag(source)
        # A core that holds nothing yet gets its box only once the drag has begun.
        box = self.target(f"core/{core}")
        self.drag_to(box.point(across, down))

    def drag_to_drop_place(self, source, core, drop_place=None):
        """Drags the named thing to a drop place of a core, the last one if none is given, and
        holds it there. Returns the drop place it is at."""
        self.drag_over_core(source, core)
        places = self.drop_places()
        if not places:
            raise DesignerError(f"core {core} shows no drop place")
        index = len(places) - 1 if drop_place is None else drop_place
        self.drag_to(places[index].point())
        return self.drop_places()[index]

    def add_processor(self, type_name, core, drop_place=None):
        """Drags a processor from the panel to a drop place of a core, the last one if none is
        given, and drops it. Returns the new processor's name."""
        names_before = set(self.processors())
        self.drag_to_drop_place(f"panel/{type_name}", core, drop_place)
        self.drop()
        new_names = set(self.processors()) - names_before
        if len(new_names) != 1:
            raise DesignerError(f"dropping {type_name} into core {core} added {sorted(new_names)}")
        return new_names.pop()

    def connect(self, output, input_port):
        """Clicks an output and then an input, each a (processor, port name)."""
        self.click(port(output[0], "output", output[1]))
        self.click(port(input_port[0], "input", input_port[1]))
