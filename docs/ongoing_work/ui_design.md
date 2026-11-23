# UI design

How the whole app looks: its colours, type, shapes and the widgets drawn from them, on every screen. What a screen does is not here: the designer's behaviour is in [pipeline_designer.md](pipeline_designer.md). This file is the look's spec followed by its open questions: a question that is settled moves up into the spec.

## Asked for
- **A 1950s atomic-age look, everywhere**, asked for by the user on 23 November 2026. The user's words that day, in order: "try to make the design of falcon ui skeumorphic, like oscilloscopes, amplifiers etc"; then "pls keep things as if they are developed in silicon valley, high level ui ux, maximum user satisfaction", "ui is shit now" and "i want 50s atomic age design everywhere, i mean everywhere, GO!".
- **Only the look changes** (the user, that day, once the work had started: "you only change ui, not ux, just the pixels and colors, no logic"). So the redesign changes how a thing is drawn, its colours, type, shapes, borders, shadows and ornament, and nothing else: which things a screen holds, where the layout puts them, what a click or a key does, every text, and all code that doesn't draw stay as they are. The tests of the UI pass unchanged.
- Claude's reading, not confirmed by the user: every screen and every widget of the app gets the look, to the standard of a commercial product; things may look like parts of an instrument of that time (a painted panel, a lamp, a knob's cap) as long as they are used exactly as before.

## State of the work
- It is done in the branch `skeuomorphic-ui`, which left `refactor` on 23 November 2026, so that it and the other work on the app go on at once.
- **The branch follows `refactor`** (the user, that day: "follow what other branch is doing and merge those changes into yourself always"): before each step of the work, and whenever `refactor` has a new commit, that branch is merged into this one. Where both changed a file, `refactor`'s logic is kept whole and the new look is put onto it again; what `refactor` adds to a screen gets the new look too. For a merge this branch's work is committed, on this branch alone and never pushed.
- A first test of the branch, the window's background in black, was seen by the user in the app that day and taken out again.
- The work goes in four steps, each by several agents (the user that day: "GO!", with a workflow asked for): (1) a list of everything the app draws and every state it can be in; (2) the design, chosen among several proposals; (3) building it, first what every screen shares, then screen by screen; (4) a review of pictures of every state, and of the code, until nothing is found. **Where it stands**: step 1 started.

## Open questions
- **The website** has a look of its own (see website.md). Whether "everywhere" means it too wasn't asked; the work is on the app alone.
- **The desktop's title bar** on top of the window isn't Falcon's to draw yet (see **No title bar of the desktop's** under **Later** in pipeline_designer.md), so it keeps the desktop's look.
