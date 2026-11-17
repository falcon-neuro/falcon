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
- **How fast a build gets**, not measured. The reused part is the precompiled header, made once on the user's machine by the bundled clang (see **The compiler's check** in generator.md). A new pipeline is still instantiated, optimised and linked each time: likely seconds, not milliseconds. Only a pipeline built before can be skipped, by keeping its engine program.
- **Installing a version other than the latest**, e.g. to repeat an earlier experiment: an argument to the website's script.
- **What `install.sh` does**: where it puts Falcon, whether it needs root, and whether two versions can sit side by side (see **Paths on Debian** and **Privileges** in beyond_prototype.md).
- **LLVM's licence text** must go with the archive.
