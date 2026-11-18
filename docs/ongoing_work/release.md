# Release

How Falcon is packed and how it reaches a user's machine, and the whole state of that work: what is decided, what is done and what is open. Every task on releases is tracked here. Nothing of it is made yet.

## Goal
Set by the user on 17 November 2026: the archive of a release, put on a fresh install of the latest stable Debian with no network, makes Falcon work flawlessly. So the archive carries everything Falcon needs that such a Debian lacks, and `install.sh` fetches nothing. The one exception is the realtime kernel (see **Decided**).

## Why a release carries its own clang
The UI compiles each pipeline on the user's machine (see generator.md). Every lab on one version of Falcon must compile with the same clang, and a system update must never change it between two sessions. LLVM's package repository can't give that: it offers only the newest build of a release branch, not a tagged release, and replaces it as the branch moves.

## Decided
By the user on 17 November 2026.
- **One archive per release**: a GitHub release of Falcon is one `.tar.gz` that holds the UI's program, Falcon's repository as a zip, the clang binaries with the libraries a pipeline's build needs, among them the system's C library headers and startup files (`libc6-dev`), and `install.sh` in its main folder. So one version number fixes everything a lab runs.
- **Two ways to install, one script**. With the network: `curl falcon-neuro.org/install.sh | bash`, a short script on the website that finds the latest release, downloads its archive and runs the `install.sh` inside it. Without the network: the user downloads the archive on another machine, extracts it and runs that same `install.sh`. No Debian package.
- **The UI builds every pipeline against the installed repository**, reusing what a first build made, so that a build after the first takes milliseconds, not 10 s (the user's aim).
- **The realtime kernel is not in the archive**: the UI has a setting, "Switch to realtime kernel", that installs Debian's realtime kernel (preempt_rt) for the user, so that a user who isn't technical needs no terminal. It is the one step that needs the network. Until the user has switched, Falcon runs a pipeline on Debian's standard kernel with a warning.
- **Not chosen**: a UI that fetches the repository and the clang itself (the user's first plan: three downloads to keep matching, and no install without the network), a Debian package depending on Debian's own clang (years older than the clang Falcon is written with), a disk image of Debian with everything in it (the most work, and the lab gives up its own installation), and our own build of clang from a release tag (a build to keep working).

## Open questions
- **What the archive holds, exactly**, a detail to settle when it is made (said by the user): beyond clang itself, at least the linker, clang's own headers, libc++ with its headers and libraries, and `libc6-dev`, which LLVM's binaries don't hold.
- **Switching to the realtime kernel, what is open**:
  - The switch needs root and a restart of the machine (see **Privileges** in beyond_prototype.md).
  - A machine that never has the network.
- **A test of the goal**, not designed: install the archive on a fresh Debian without network, e.g. in a virtual machine, then build and run a pipeline.
- **Where the clang binaries come from when a release is made**: a small repository of its own, proposed first by the user, which cuts LLVM's official binaries of one tagged release (about 2.5 GB) down to what a pipeline's build needs (tens of MB hoped) and publishes them, or the release's own build doing that cut. Either way the release names LLVM's version and checks the download against its checksum.
- **A pipeline's build without the UI's libraries**, parked by the user on 18 November 2026, not urgent: users build pipelines over and over, and that build must not fetch and compile what only the UI needs (GLFW, Dear ImGui, glad), as the one `CMakeLists.txt` does today. Proposed by the user, not designed: a `CMakeLists.txt` of its own for pipelines, or an argument to CMake that leaves the UI's libraries out.
- **How fast a build gets**, not measured. The reused part is the precompiled header, made once on the user's machine by the bundled clang (see **The compiler's check** in generator.md). A new pipeline is still instantiated, optimised and linked each time: likely seconds, not milliseconds. Only a pipeline built before can be skipped, by keeping its engine program.
- **Installing a version other than the latest**, e.g. to repeat an earlier experiment: an argument to the website's script.
- **What `install.sh` does**: where it puts Falcon, whether it needs root, and whether two versions can sit side by side (see **Paths on Debian** and **Privileges** in beyond_prototype.md).
- **LLVM's licence text** must go with the archive.
- **GCC in place of clang**, decided by the user on 18 November 2026 for the codebase, for reflection, and not yet worked out for the release (see **Clang for development, GCC to compile** under **No reflection soon** in the open questions of engine.md, with what was checked). If it holds, this file changes with it: the compiler the archive carries, its C++ library in place of libc++, and its licence text. Wanted by the user: a script in the repository that fetches one exact, official GCC, without building it. Where the binaries come from, weighed by Claude that day, not decided:
  - GCC's own project publishes source only, so the one official binary is a distribution's. Debian's packages of GCC 16.2.0 (version 16.2.0-4, in Debian testing) need no newer C library than Debian 13 has, and snapshot.debian.org keeps every version of them for good, so a script can fetch exactly those files and check each against a checksum it holds.
  - **Installed with apt**: the proper way. But it replaces the system's C++ runtime libraries, about 12 packages, with those of testing, for every program of the machine; they no longer get the stable release's security updates; and a wrong priority for testing's packages lets the next upgrade take the whole system to testing.
  - **Unpacked into a folder of Falcon's**, not installed: nothing of the system changes, and it is the form the archive needs. Chosen by the user on 18 November 2026 as the one to try first: `scripts/setup/fetch_gcc.sh` fetches the 19 packages (72 MB), checks each against its checksum, unpacks them and, as the trial, compiles and runs a small program with the unpacked compiler. Run by the user that day, and it works: GCC, though built to sit in `/usr`, finds its own parts from that folder, and the program it built used reflection.
  - **Cutting the compiler down to what a pipeline's build needs**, parked by the user on 18 November 2026 as a task of the release: unpacked, the 19 packages take 249 MB, of which the C compiler and the link-time optimiser, about 80 MB, are likely not needed (not tried).
