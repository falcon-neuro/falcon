# Test plan

How Falcon is tested. The prototype spec is in [spec.md](../core/spec.md); work after the prototype is in [beyond_prototype.md](../core/beyond_prototype.md).

## Tests as whole runs
Paused, with no choice made yet between the plan below and existing tools. Its place in the order of work is not set (see the top of beyond_prototype.md). No unit tests. Each folder under `src/tests/` gets a `run.py` that builds its test pipelines, runs them and judges each run from outside: by its log lines, by a comparison with SciPy, or by a file the run wrote (e.g. a recording). `src/tests/run_tests.py` runs every folder's `run.py` as a process of its own and reports which passed.
- Agreed: Python, not bash, whose checks on numbers and log lines are fragile; the IirFilter test's comparison is Python already.
- Agreed: one Python environment for all tests, in `src/tests/` (`pyproject.toml`, `uv.lock`), not one per folder. So the lint must skip that environment's folder, and `__pycache__` must be git-ignored. Not done yet; `pyproject.toml` and `uv.lock` are still in `iir_filter/`.
- Open: existing tools instead of our own. pytest already does what `run.py`, `run_tests.py` and the shared helper would (finding tests, skipping, selecting, reporting, with the helper as a `conftest.py`); CTest, which comes with CMake, runs a test per build target and judges it by exit code or a pattern in its output. LLVM's FileCheck matches log lines against expected lines written as comments in the test's `.cpp`, but can't check a sum. Only the test pipelines, the checks and a small helper that knows Falcon (build, run, stop, find the run's directory, read its log) would be ours. Leaning to pytest, starting with the two folders that exist.
- Agreed: an extension's tests live in it, in `custom/<extension name>/tests/`, like its pipelines (see **Extensions** in spec.md), since a public test can't include a private processor.
- To design: whether `run_tests.py` also runs the tests of every extension it finds.
- To design: how a test's program gets built, since the build has one `falcon` target whose source is set by hand in `CMakeLists.txt`. Three ways, leaning to the first:
  - One CMake target per test program, with each extension declaring its own in its `CMakeLists.txt`. Nothing is edited to switch tests, the `falcon` target stays the developer's, and the lint gets each test's real compile flags. But `CMakeLists.txt` must be restructured, since every option hangs on `falcon`, and a naming rule must tell a test program from a helper file.
  - The single target, with its source passed as a CMake variable. A few lines, but every switch re-runs CMake and changes what `falcon` is.
  - The helper calling clang directly, as the UI will for a generated `pipeline.cpp`. No CMake change, but the flags then live in two places, and an extension's libraries must be passed by hand.
- To design: a helper the `run.py` files share (build, run, stop after some seconds, find the run's directory, read its log).
- To design: how a check deals with what differs between runs: times, the run's directory, and counts that depend on timing.
- To design: what a test needs from the machine (isolated cores, hardware, root), so a test that can't run is skipped, not failed.

## Processor tests
Was parked for after the prototype; a candidate to pull into the prototype, not confirmed yet (see **Parked too early** in the open questions of spec.md).
- The first: IirFilter against SciPy, in `src/tests/iir_filter/`. Its `README.md` describes the test, its results and what is still to test.
- Each test runs both versions of the processor, AVX512 and AVX2 (see **One processor per stream** under **Processors** in spec.md). An AVX2 build runs on any supported CPU; an AVX512 build needs an AVX512 CPU or an emulator (e.g. Intel's SDE).
- Every processor gets tests. They catch mistakes the processor interface doesn't prevent, e.g. a sample processor that never calls `set_header`, so its output is always empty and everything downstream stays silent (see **Operations a processor uses** under **Buffers** in spec.md).
- Each processor is also run with every optional connection left out, since using a missing one compiles and is undefined behaviour (see **Required and optional connections** under **Processors** in spec.md).
- The tests also matter for linting: clang-tidy analyzes a processor's code only where something instantiates its template, so a processor that no pipeline and no test instantiates is never checked.

## Open questions
**What else moves here.** Still where they were, each about testing or measuring:
- The logger's test pipelines (**Test pipelines** under **Logging** in spec.md).
- A detector's test: RippleDetector's test pipeline and test signal, and **Judging a detector with running statistics** (both under **Porting Falcon 1's processors** in the open questions of spec.md).
- Latency measurement: **How we judge that it works** under **Agreed** and **How to measure the round trip** in the open questions of spec.md, and **Latency tests for the paper** in beyond_prototype.md.
- **Sanitizer builds** under **Hardening** in beyond_prototype.md.

**A state across cores in a test.** A change of such a state isn't seen on the same sample in every run (see **Across cores a reader gets the latest value, without waiting** under **Buffers** in spec.md), so a test can't expect its effect on an exact sample. Open: whether such a test accepts a range of samples, or keeps the state's producer and reader on one core. The first such test, `src/tests/state_values/state_values_closed_loop_test.cpp`, needs neither: it is judged by its Ripple events coming a second apart, where a few samples either way don't show. It has no `run.py` yet: its first comment states the limits, and its log is checked against them by hand.
