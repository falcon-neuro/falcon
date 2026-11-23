# Security

What could make Falcon run code, or use rights, that its user didn't intend, and the whole state of that work: what is decided and what is open. Every task on security is tracked here. Nothing of it is built.

## Decided
- **Everyone may write the repository's folder, and that is accepted** (the user, 21 November 2026). The engine's source lies in a shared folder at the machine's root (see **The UI fetches the repository** in release.md), and the engine program needs root today (see **Privileges** in beyond_prototype.md), so source that anyone may change is code that anyone can have run as root. The user's reason: a malicious program under the user's own name could change the source in a home folder just as well, so the shared folder adds nothing that matters.

## Open questions
Parked by the user on 21 November 2026, so as not to go down a rabbit hole: none of these is being worked on, and what is under **Decided** stands until the topic is taken up again.
- **Whether source can be changed on the disk while the git hash stays the same**, asked by the user on 21 November 2026. Claude's answer, not yet confirmed as Falcon's stance: yes.
  - The commit hash names the commit that is checked out, not what the files hold: a file that is changed and not committed leaves the hash as it is. The check for modified source shows such a change (see **A repository with changes that aren't committed** in generator.md).
  - Whoever may write the folder can make that check blind. Git can be told to pass over a file's changes, and git's own data, which the check reads, lies in the same folder.
  - What the commit doesn't cover at all, and the build still uses: the build's folder with its intermediate files, the extensions, and the compiler.
  - So the hash and the check guard against accidents and honest changes, never against a malicious party. Against one, only what it can't write helps: source that it can't write, or a checker that is root's, hashes the files themselves and compares with a hash that it didn't read from the folder.
- **What Falcon defends against**, not decided: accidents alone, which the hash and the check do, or a malicious party too.
- **How to make sure a malicious party can't alter the engine**, asked by the user on 21 November 2026, who added that extension developers would need the hardening lifted to modify the framework. Claude's answer, not decided; it would take the place of **Everyone may write the repository's folder** under **Decided**:
  - Only ownership ensures it: the engine's source, the compiler and whatever the build keeps for reuse belong to root and are read-only for everyone else, so no program under an ordinary user's name can change them. A hash or a check only shows a change, and whoever may write can blind it (see the first question).
  - A pipeline's build then writes its intermediate files in the user's own folder, not in the repository.
  - It doesn't hold against whoever has an administrator's rights on the machine: nothing on the same machine does.
  - For developers, a developer mode that an administrator unlocks: Falcon then builds from a copy of the repository that the developer may write, and what is built from it is marked as built from a modified engine (see **A repository with changes that aren't committed** in generator.md). Locked is the default for everyone else.
  - It is worth nothing while the engine runs as root: `pipeline.cpp` and the executable lie in the user's own folder and are code as well, which a malicious program under the user's name can change and have run as root. So the engine running without root comes first (see **Limiting the harm of changed source** below).
- **Limiting the harm of changed source**, asked by the user on 21 November 2026 ("how do we mitigate this?"). Claude's answer, not decided, and whether it is part of the prototype is open:
  - The engine program runs as the user and holds only the rights it needs (see **Privileges** in beyond_prototype.md). A changed engine can then do only what those rights allow, though the one for I/O ports is itself far-reaching.
  - The step that grants those rights needs root at every build, so it can be the guard: a small helper that is root's and lies outside the shared folder grants them unasked only to a program built from a released commit with nothing modified, and asks for an administrator's password otherwise. It is only as good as its check of the source (see the first question above).
