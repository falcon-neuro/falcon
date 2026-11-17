#!/usr/bin/env python3
"""Runs the tests of the pipeline designer, each against a Falcon of its own whose window isn't
shown (see designer_driver.py). Falcon has to be built first: sh scripts/make.sh.

usage: python3 src/tests/ui/pipeline_designer/run.py [part of a test's name ...]

Without arguments every test runs. A test is a function named test_... in a file named test_...
in this folder. The exit code is 0 when no test failed that wasn't expected to.
"""
import importlib
import sys
import time
import traceback
from pathlib import Path

FOLDER = Path(__file__).resolve().parent
sys.path.insert(0, str(FOLDER))
import designer_driver  # noqa: E402  (after the path it is found on)

FAILURE_PICTURES = designer_driver.REPOSITORY / "build" / "ui_test_failures"


def tests_to_run(name_parts):
    for path in sorted(FOLDER.glob("test_*.py")):
        module = importlib.import_module(path.stem)
        for name, test in list(vars(module).items()):
            full_name = f"{path.stem}.{name}"
            is_wanted = not name_parts or any(part in full_name for part in name_parts)
            if name.startswith("test_") and callable(test) and is_wanted:
                yield full_name, test


def main():
    name_parts = [argument for argument in sys.argv[1:] if not argument.startswith("-")]
    if len(name_parts) != len(sys.argv) - 1:
        sys.exit(__doc__)
    FAILURE_PICTURES.mkdir(parents=True, exist_ok=True)
    counts = {"passed": 0, "failed": 0, "known failures": 0, "no longer failing": 0}
    for full_name, test in tests_to_run(name_parts):
        known_failure_reason = getattr(test, "known_failure_reason", None)
        designer_driver.PICTURE_ON_FAILURE = FAILURE_PICTURES / f"{full_name}.png"
        start = time.monotonic()
        try:
            test()
            error = None
        except Exception:  # a test may fail in any way, and the others still run
            error = traceback.format_exc()
        seconds = time.monotonic() - start
        if error is None and known_failure_reason is None:
            counts["passed"] += 1
            print(f"PASS           {full_name} ({seconds:.1f} s)")
        elif error is None:
            counts["no longer failing"] += 1
            print(f"NO LONGER FAILS {full_name}: marked as a known failure ({known_failure_reason})")
        elif known_failure_reason is not None:
            counts["known failures"] += 1
            print(f"KNOWN FAILURE  {full_name}: {known_failure_reason}")
        else:
            counts["failed"] += 1
            print(f"FAIL           {full_name} ({seconds:.1f} s)")
            print("    " + error.rstrip().replace("\n", "\n    "))
    print(", ".join(f"{count} {what}" for what, count in counts.items()))
    sys.exit(1 if counts["failed"] or counts["no longer failing"] else 0)


if __name__ == "__main__":
    main()
