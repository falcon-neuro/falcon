# Website
`docs/` is the websites' source. This file is internal: only what is under `docs/pages/` is published.

## Goal
Three websites, each linking to the other two:
- **Main**: the landing page a visitor sees first, made to impress.
- **Documentation**: the pages under **Pages**, reached from the main website. It reads as the documentation of a finished product. Its full page structure exists from the start; a page about something that isn't built yet says "coming soon" until it can be written.
- **API**: the reference generated from the code (classes, files, C++ concepts), for whoever writes a processor.

Work on the websites changes only `docs/`, the scripts that build them and, where the build needs it, `.gitignore` and the deployment workflow.

State: the three websites are built and link to each other (see **How they are built**). The main website is one placeholder page so far.

## How they are built
`scripts/doc_gen.sh` builds all three into `docs/build/` (git-ignored), which the workflow `.github/workflows/deploy_website.yml` publishes to GitHub Pages on a push to `main` or `develop`. The script needs no `sudo`: it downloads Doxygen and Doxygen's theme, at the versions it names, and the Python packages listed in `docs/requirements.txt` (Zensical among them) into the git-ignored `build/`. When that list changes, it installs them again. It stops at the first problem.

1. **API**: Doxygen reads `src/` with `docs/doxygen/Doxyfile` and writes the API's pages into `docs/pages/api/` (git-ignored) and a tag file, `build/falcon_api.tag`: each class's name and its page's file. `docs/doxygen/api_main.md` is the API's main page; it links to the other two websites.
2. **Main website and documentation**: Zensical reads `docs/zensical.toml` and everything under `docs/pages/`, and writes `docs/build/`. It renders the Markdown pages and copies every other file unchanged, the API's pages included. The top bar's tabs are "Home", "Documentation" and "API".

Addresses: the main website at the root, each documentation page directly under it (e.g. `installation/`), the API under `api/`. Every link between them is relative, so the websites work under any address.

### Class links
In a page, `{{ api("ChannelMask") }}` becomes a link to that class's page on the API website. It takes a class, struct or concept by its full C++ name. `docs/main.py` defines it, as a Zensical macro that looks the name up in the tag file; that is why the API is built first. A name the API doesn't have fails the build.

With macros on, Zensical reads `{{`, `{%` and `{#` in a page as macro syntax: a page that needs them as plain text must escape them.

### Search engines
Goal: Google presents Falcon as a piece of software. "Falcon" alone is taken by far larger names (a rocket, a security product, a Python web framework), so the realistic target is a search with a second word, e.g. "falcon closed loop" or "falcon neuroscience". Google alone decides whether to show a panel about Falcon, and its special result for software needs user ratings or reviews, which Falcon has none of and must not invent.

- The public address, https://falcon-neuro.org/, is `site_url` in `docs/zensical.toml`. Zensical derives each page's canonical address and `sitemap.xml` from it.
- Every page starts with a `description` in its front matter: the text under its title in search results. A page without one gets the website's description.
- `docs/overrides/main.html` adds to every page what a shared link shows (title, description, the image `docs/pages/assets/social_card.png`), gives the main page the title "Falcon: open-source software for closed-loop neuroscience", and adds to the main page a description of Falcon as software in schema.org's vocabulary: name, category, Linux, GPL v3, free of charge, the repository, the 2017 paper.
- `docs/pages/robots.txt` names the sitemap and keeps search engines off the API, whose pages hold little text while the code has few doc comments.

### Formulas
A page writes a formula in LaTeX: `$...$` within a line, `$$...$$` on lines of its own. The build turns it into MathML, which browsers display themselves, so a formula needs no JavaScript and no outside library. Two Python packages do it, listed with Zensical in `docs/requirements.txt`: `latex2mathml` converts, `l2m4m` makes Markdown call it.

Traps when writing one:
- Several aligned lines take `\begin{align*}`. `\begin{aligned}` isn't understood and comes out as one line.
- Within a line, Markdown reads a backslash before a punctuation mark (`\{`, `\\`, `\_`) before the formula does. Put such a formula on lines of its own.
- `{{` starts a macro (see **Class links**), also inside a formula: write `{ {`.
- A `|` inside a table cell ends the cell.

### Glossary
The glossary is the page `docs/pages/glossary.md` itself, which the engine's docs and the work on them use as their list of terms too, so the glossary exists once. After its description, the title is followed by the terms, with no introductory sentence.

- What is written in it is therefore published. Its text passes through the macros and the formulas like any page's: `{{`, `{%`, `{#` and a pair of `$` have their meaning there too (see **Class links** and **Formulas**).
- No page includes a file any more, so `include_dir` in `docs/zensical.toml`, still `../src/core`, is unused and could go. Used again, it stays a small folder, since Zensical reads through all of it on every build: set to the repository's root, a build took 80 seconds and then failed on a file it couldn't read.

### Fonts
The text is set in Inter, from `docs/pages/assets/fonts/inter_latin.woff2`: the repository's own Inter file (`assets/`), reduced to Latin characters, 97 kB. Code is set in the visitor's own monospace font. `font = false` in `docs/zensical.toml` stops Zensical from loading fonts from Google.

### Search dialog
Zensical draws its search dialog over a blur of the whole page behind it. A browser that draws without a graphics card (hardware acceleration switched off, a virtual machine, a remote desktop) computes that blur again for every frame while the dialog is open, so the whole website turns slow as soon as the search opens. `docs/pages/javascripts/search_dialog.js`, which `extra_javascript` in `docs/zensical.toml` loads on every page, removes the blur and makes the dialog's panel opaque, since the page's text would otherwise show through it. The veil over the page stays translucent.

The dialog sits in a shadow root that Zensical's script creates, out of reach of `extra.css`: hence a script, which adds its rules to that shadow root.

Trap: the rules find the panel by the dialog's structure (its children are an empty veil and the panel) and colour it with Zensical's colour variables, and a later Zensical may change both. After changing Zensical's version, open the search and check that the panel is opaque and that nothing behind it is blurred.

## Local preview
Whoever works on the websites keeps them built and served locally the whole time, and gives the address when reporting a change, so each change can be looked at in a browser. From the repository's root:

1. Build everything, once per machine and again whenever the code's doc comments change:
   ```sh
   sh scripts/doc_gen.sh
   ```
2. Serve, once per session, as a background process:
   ```sh
   build/website_env/bin/zensical serve -f docs/zensical.toml -a 127.0.0.1:8765
   ```

The three websites are then at http://127.0.0.1:8765. Saving a page or `docs/zensical.toml` rebuilds them; the server's output says "No issues found" or names the problem. The server stops on a mistake in a macro call (see **Class links**), and has stopped once when a file under `docs/pages/` was deleted while it ran: correct the mistake and start it again.

Both steps at once: `sh scripts/doc_serve.sh [port]`, port 8765 unless another is given, or the `serve docs` task in VS Code.

Doxygen's warnings about undocumented members in the code are expected.

## Pages
One Markdown file per page, in `docs/pages/`, in plain Markdown with no Doxygen commands. `docs/zensical.toml` lists them, and its list sets their order in the sidebar. `index.md` there is the main website's page, a placeholder so far.

`docs/pages/assets/` holds the browser-tab icon, the image a shared link shows (the falcon, the name and one line, on a dark ground) and three screenshots (two of the graph editor, one of the live signal viewer) taken from the FENS 2026 poster's files, which aren't in this repository. They are temporary, until proper assets exist. The poster's latency plots were measured on the old engine and are never used.

| # | Page | File | Content | State |
|---|------|------|---------|-------|
| 1 | Overview | `overview.md` | What Falcon is, who it is for, what it can do; the list of pages, closed by the website's one "coming soon" sentence; **Origins** as its last section. | First version, to rewrite. |
| 2 | Processor reference | `processor_reference.md` | The list of the processors that have a page, each linking to it under `processors/`. With those pages it forms the "Processors" group in the sidebar. | Lists two. |
| 2a | IIR filter | `processors/iir_filter.md` | See **Processor pages** under **Open questions**. | Written and proofread against the code and the glossary. |
| 2b | Spike detector | `processors/spike_detector.md` | As 2a. | Written and proofread against the code and the glossary. |
| 3 | Glossary | `glossary.md` | The page itself (see **Glossary** under **How they are built**). | Done. |

Pages to come, each added only once it has content: How it works (graph, processors, engine, and where the latency goes), System requirements (CPU, Debian with preempt_rt, core isolation, supported acquisition hardware), Installation, First experiment (building a graph in the editor and running it), Example experiments (spike detection, ripple detection, replay decoding), Writing a processor, and a page per remaining processor. Until then one sentence in the overview names them. When one is added, take it out of that sentence.

Next: the remaining processor pages (see **Open questions**), each agreed at a high level before it is written.

## Documentation from the code
Once Falcon is a usable product. The website is generated by Doxygen from two sources only, both kept minimal: doc comments in the code, and Markdown pages, such as tutorials on using Falcon. Until then its pages follow `.claude/ongoing_work/website.md`: the full page structure, with "coming soon" on what isn't built yet. The engine's docs, the test suite's and that plan lie outside its sources, in `.claude/ongoing_work`, as the brochure's notes do in `docs/poster`; the glossary is one of its pages, `docs/pages/glossary.md`. A push to `main` or `develop` deploys the website (`.github/workflows/deploy_website.yml`); these pages and the README without its install command are not on `develop` yet, so the live website still shows the old ones. Doc comments then also describe for users what each processor, option and connection does, a kind of comment beside the ones for what the code can't say.
- For processor authors: when an event is a channel mask (it says which channels did something on one sample, with no value per channel, e.g. spikes, level crossings) and when a sparse event (it carries a value per channel or per group, e.g. an amplitude, a tetrode's marks, a waveform, or isn't about channels, e.g. a trigger, a timer, a decoder result). See **Detection events are channel masks** in engine.md.

## Decided
- Three websites, not one (see **Goal**). Nothing generated from the code, e.g. the "Concepts" entry, appears on the main or the documentation website.
- The API website keeps Doxygen's own pages with the doxygen-awesome-css theme, as they are today. At most ten people will ever read it, so it gets no further effort on its look.
- The effort goes to the main website and the documentation, which must look excellent. The documentation leaves Doxygen for that. Looks rejected so far: doxygen-awesome-css, m.css (Magnum's documentation), Poxy (toml++'s). Chosen: Zensical, approved on Falcon's own pages. Second choice: the Shibuya theme for Sphinx, as on https://shibuya.lepture.com/customisation/announcement/.
- The logo is the falcon alone, never inside a disc (the disc version is the Debian desktop icon): dark in light mode, pure white in dark mode, on all three websites. The browser-tab icon is the falcon alone too; it follows the system's light or dark setting, since a tab icon can't follow the website's switch.
- Light and dark mode: a website opens in the system's mode, and its switch then goes only between light and dark. There is no third "system" choice.
- The public address is https://falcon-neuro.org/.
- For search engines, only what the websites themselves can do. Nothing outside the repository (Wikidata, research-software registries, a DOI): too much for now. No Google Search Console either, as there is no Google account: Google finds the new pages by itself, from the address it already knows and the sitemap that `robots.txt` names.
- The websites are open source and free of bloat: built only from open-source parts, and a visitor's browser loads nothing from an outside server (no Google Fonts, no library from a content delivery network).
- No Node and no JavaScript toolchain in the websites' build.
- The pages don't show the "Made with Zensical" line (`generator = false` in `docs/zensical.toml`, a switch Zensical's theme provides), nor the grey strip that held it: `docs/pages/stylesheets/extra.css` hides the strip, which would also hide a copyright line or social links if either were set later.
- A class named in a documentation page links to that class's page on the API website automatically: nobody writes the address by hand.
- The website carries only content that is needed, and no page without it. "License and contributing" is deleted: the overview names the licence and links to the repository. The six pages that said only "coming soon" are deleted too. "Coming soon" may be said, but once on the whole website (see **Pages**).
- No machine-sounding filler: no sentence that only announces what follows, no promise of content in a page's search description that the page doesn't hold, no bold lead-in on each item of a list.
- The maintainer merges and deploys. Whoever works on the websites never commits, merges, pushes or deploys.
- No page for users coming from Falcon 1, since only its authors ever used it.
- The website honours the original Falcon and its paper, but in the last section of the overview (**Origins**), never at the entrance: the website looks forward. For now it lists only the 2017 paper. Papers that cite or used Falcon come later; the first of them is the 2018 eLife paper (Ciliberti, Michon, Kloosterman, https://doi.org/10.7554/eLife.36275), which the earlier single page listed.

## Verified
On one development machine, with Zensical 0.1.1 and Doxygen 1.18.0. The deployment workflow hasn't run with the new script yet.
- `scripts/doc_gen.sh` runs to the end and `docs/build/` holds the three websites; one page of each loads from the local server, also right after the server starts.
- After the glossary became its page and Doxygen's two exclusions for notes under `src` were removed, since those notes left `src`: the build has no issues, the glossary page shows its 27 terms, and none of the internal notes is in the build. `scripts/doc_serve.sh` serves the three websites.
- The pages build with no issues, in under a second. Under the "Documentation" tab the sidebar shows "Overview", the "Processors" group and "Glossary".
- The class links on the two processor pages lead to those classes' API pages. With a name the API doesn't have, the build fails.
- The top bar's link to the repository no longer asks GitHub for the release, the stars and the forks: `docs/overrides/partials/source.html` replaces the theme's version of the link, without the mark that made the theme's script ask. Checked in the built pages and in a screenshot, not by watching the browser's requests.
- Zensical takes the folder of its configuration file as the project's root and refuses an output folder outside it. Hence the output in `docs/build/`.
- A build by `zensical build` or by the server's start empties `docs/build/` first. Hence the API's pages in `docs/pages/api/`, from where every build copies them, rather than added to the output afterwards.
- The logo is one file for all three websites, `docs/overrides/.icons/falcon/logo.svg`. Zensical places the file's content in the page itself, where it takes the text's colour; `docs/pages/stylesheets/extra.css` makes it pure white in dark mode. The API's header (`docs/doxygen/header.html`) uses the file as a mask over a colour that `docs/doxygen/falcon_api.css` sets. Shown as a plain image it could not change colour. Checked in both modes on the documentation and on the API.
- The browser-tab icon is `docs/pages/assets/favicon.svg`, for all three websites. It holds a second copy of the falcon's outline, since a tab icon can't read another file.
- The light and dark switch, checked with a browser set to light and one set to dark: each opens in its own mode and shows a single sun or moon button. Clicking the button wasn't tested by a tool.
- Doxygen reports that `latex` is missing and can't render the formulas in the code's doc comments. It still finishes, and the build doesn't stop for it.
- The API's tag file lists 19 classes, 120 structs and 5 concepts.
- Search engines: the built pages carry the titles, descriptions, canonical addresses and shared-link tags; the main page's software description parses as valid JSON; `sitemap.xml` lists the six pages. Not checked with Google's own testing tool, which needs the published website.
- The IIR filter's page shows its 23 formulas as MathML with none failed, in light and dark mode, in Firefox on Linux. Not looked at in Chrome or Safari, nor on a machine without a mathematical font.
- The glossary page shows all 27 terms.
- The built documentation pages reference no outside server except as links (the repository, the two papers' DOIs).
- Zensical leaves `page.is_homepage` unset in its templates, so `docs/overrides/main.html` recognises the main page by its address.
- https://falcon-neuro.org/ still served the old Doxygen website on 2026-11-16.
- The search dialog (see **Search dialog** under **How they are built**), measured in a headless Brave, which draws without a graphics card, in a window of 2520 by 1303, while typing "filter coefficients", twice each way. Without the script's rules, the screen followed a key press after 80 to 88 ms on average, 127 ms at worst, and 47 to 48 frames took over 40 ms. With them: after 30 ms, and no such frame, also after following a link to another page. The page's other blurs (e.g. the top bar's) stay and cost nothing measurable while searching. The search itself was never the cost: in 6 s of typing, the page's script ran for 0.15 s, and the search data is 27 entries in 21 kB. With the script the panel is opaque and readable in light and dark mode, checked in screenshots. Confirmed on 2026-11-16 in the browser the slowness was reported from, a Brave with hardware acceleration off.

## Open questions
- **Search dialog, what is left.** The slow search is fixed (see **Search dialog** under **How they are built**, and **Verified**). Two points are open with it:
  - The look: without the blur the page stays sharp behind the veil, around the panel. The veil could be made more opaque.
  - The way: the other way was `scripts/doc_gen.sh` deleting the blur from Zensical's installed script. It puts no script of ours in the pages and fails the build when a later Zensical no longer matches, but it changes an installed file of someone else's, whose name stays the same, so a browser may keep the old one.
- **`include_dir`, unused.** Nothing is included any more (see **Glossary** under **How they are built**). Remove the setting from `docs/zensical.toml`, or keep it for a later include?
- **Search engines, what is left.** The websites' own part is built (see **Search engines** under **How they are built**). Left open: who the software description names as Falcon's maker (a lab, an institute?), which it leaves out so far; and whether the API should be searchable after all.
- **One light or dark choice for all three.** The documentation and the API each remember the visitor's choice separately, so switching one to dark leaves the other as it was. Make them share it? It would need a few lines of JavaScript of ours in the API's header.
- **Main website's page.** Next to design. Zensical's documentation says a page can use a template of its own from an overrides folder, with extra CSS. So `index.md` would get a hand-written template under `docs/`, for full freedom in visuals, while sharing the documentation's top bar, colours and fonts. Not tested.
- **Deployment, to watch.** The first push to `main` or `develop` runs the new `scripts/doc_gen.sh` on GitHub's machine for the first time. There it installs Zensical with `python3 -m venv` and `pip`, a path never run so far (the development machine used `uv`). The workflow also still clones `falcon-fklab-extensions` before the build, which the build no longer reads: remove that step?
- **Documentation's address.** Its pages sit directly under the root (see **How they are built**). They could move under `docs/` by moving their files one folder down; cheap now, a broken link for every visitor once published.
- **Zensical's maturity.** It is free under the MIT licence but at version 0.x: its interfaces may still change, so `scripts/doc_gen.sh` pins its version. It runs no plugin of ours: its own module interface is in development and planned to reach the members of its makers' paid product Spark first. The class links don't need it. Fallback if Zensical ever fails us: Sphinx with the Shibuya theme (see **Decided**), where an existing extension (sphinxcontrib-doxylink) resolves class links.
- **Processor pages.** Wanted: a lot of content on each built-in processor, with formulas. Two are written, each from its processor's code: the IIR filter and the spike detector. Their form: what it does; input and output; options; what it computes, with formulas; what it does at its first sample and after lost samples; what Falcon checks before a graph runs; delay; how to choose its option values, with a worked example; its classes in the API. Their words follow the glossary (`docs/pages/glossary.md`): a sample is all channels at one time, one channel's number is a channel value, a processor has inputs and outputs (a connection is the link between two processors), and "rate", not "sampling rate". The other eleven follow the same form, each written only once its code has been read: event generator, event logger, FIR filter, latency benchmark, level crossing detector, Neuropixels input, PCIe parallel output, recorder, ripple detector, signal generator, trigger. Left out for lack of facts: how a user sets the options (the editor isn't described anywhere yet) and what a processor costs in computing time (not measured).
- **Main page, a placeholder.** It holds a title, one line, one screenshot of the old editor and a link. Not filler, but not the front door that was asked for (see **Main website's page**).
- **Formulas in the API.** Doxygen can't render the formulas in the code's doc comments without LaTeX installed (see **Verified**). Install LaTeX where the websites are built, or leave them unrendered? Doxygen's other way, MathJax, loads a library from an outside server, which **Decided** rules out.
- **JavaScript in the pages.** Node is out of the build (see **Decided**); whether the pages themselves may run JavaScript is open. Without any: no search box, no dark-mode switch (the pages still follow the system's setting), no copy buttons, and the main website moves by CSS and SVG only. Zensical's search is JavaScript that it ships. One script of ours exists already, for that search's dialog (see **Search dialog** under **How they are built**).
- **Main website's content.** Its sections, and what it claims about latency while nothing is measured yet: the target, or no number until the benchmarks exist. The poster's closed-loop drawing (brain, acquisition, Falcon, stimulation) is unused so far because it carries the claim "≤ 1 ms".
- **What the website lacks.** Candidates, none decided, to raise one at a time:
  - **Measured latency.** A benchmarks page: the latency distribution (median, 99th percentile, worst case) of named graphs on named hardware, with the method, so anyone can repeat it. Coming soon until the engine can be measured.
  - **Pictures.** A screenshot or short clip of the graph editor, the processor icons (`assets/`), a drawn graph per example experiment, a drawing of where the latency goes.
  - **Where Falcon fits.** When to choose Falcon, an FPGA, or another open-source tool (e.g. Open Ephys, Bonsai, RTXI, BRAND), including when not to choose Falcon.
  - **Hardware support table.** Each acquisition system with its state (works, in progress, planned), in System requirements.
  - **Trust signals.** A link to the repository, the build's state, releases with a changelog, a roadmap, a place to ask questions, how to cite (text to copy, `CITATION.cff`), the labs and funders behind Falcon, papers that used it.
  - **Research use only.** A statement of whether Falcon may be used on patients.
  - **Data out.** What Falcon records and how to open it in Python (e.g. NWB, SpikeInterface).
  - **Troubleshooting.** A page of common problems.
  - **Website mechanics, to audit.** Dark mode, search, phone layout, copy buttons on code, the image and description a shared link shows, an "edit this page" link, `llms.txt` and plain-Markdown pages for AI assistants, documentation per released version.
- **Citing.** Does the website ask users to cite the 2017 paper?
- **API website.** The top bar's "API" tab links to it now. With few doc comments in the code, does the tab stay, or is the API built but left unlinked until the doc comments exist (see **Documentation** in `.claude/ongoing_work/beyond_prototype.md`)? A build on a developer's machine also includes the git-ignored extensions under `src/processors/custom/`, e.g. a vendor library's headers, which the deployed website doesn't have.
