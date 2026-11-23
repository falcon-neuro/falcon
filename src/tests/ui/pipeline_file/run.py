#!/usr/bin/env python3
"""Runs the test of the pipeline's writer and reader, pipeline_file_test.cpp in this folder. It is
a program of its own, which the build makes next to Falcon: sh scripts/make.sh.

usage: python3 src/tests/ui/pipeline_file/run.py

The exit code is 0 when no test failed.
"""
import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parent.parent.parent.parent.parent
TEST_PROGRAM = REPOSITORY / "build" / "pipeline_file_test"
# The test only writes and reads texts; this is for a program that hangs.
SECONDS_FOR_THE_RUN = 60


def main():
    if len(sys.argv) != 1:
        sys.exit(__doc__)
    if not TEST_PROGRAM.exists():
        sys.exit(f"{TEST_PROGRAM} is missing: build first, with sh scripts/make.sh")
    # The repository's folder, for the tests that read the pipelines in it.
    command = [TEST_PROGRAM, REPOSITORY]
    sys.exit(subprocess.run(command, timeout=SECONDS_FOR_THE_RUN, check=False).returncode)


if __name__ == "__main__":
    main()
