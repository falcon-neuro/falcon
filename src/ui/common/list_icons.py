"""Writes the two lists of every icon in assets/remixicon.ttf: icon_list.cpp for the UI's code, and
remixicon_showcase.html to search them in a browser.

Run by hand when the font is replaced by another release, on the remixicon.css of that release:
    python3 src/ui/common/list_icons.py --css <release>/fonts/remixicon.css \\
        --output src/ui/common/icon_list.cpp --showcase assets/remixicon_showcase.html
"""
import argparse
import json
import pathlib
import re

ICON_RULE = re.compile(r'^\.ri-([a-z0-9-]+):before \{ content: "\\([0-9a-f]{4})"; \}$', re.MULTILINE)
RELEASE_LINE = re.compile(r"^\* Remix Icon v([0-9.]+)$", re.MULTILINE)
# Later releases state another licence on this line, one that is not open source.
LICENCE_LINE = "* Released under the Apache License Version 2.0"
# The font's whole range, which fonts.cpp keeps free of the text font's own glyphs.
PRIVATE_USE_AREA = range(0xE000, 0xF900)

# The fonts are found beside the page, so it only works from the folder that holds them.
SHOWCASE_PAGE = r"""<!doctype html>
<!--
Written by src/ui/common/list_icons.py from Remix Icon @RELEASE@, https://remixicon.com.
Copyright RemixIcon.com, released under the Apache License Version 2.0, which is in
remixicon_license.txt.
Not to be edited: run the script again on the release of remixicon.ttf.
-->
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Remix Icon @RELEASE@</title>
<style>
@font-face {
  font-family: "remixicon";
  src: url("remixicon.ttf") format("truetype");
}
@font-face {
  font-family: "Inter";
  src: url("Inter-VariableFont_opsz,wght.ttf") format("truetype");
  font-weight: 100 900;
}
:root {
  --page: #f6fbf3;
  --card: #ffffff;
  --ink: #0d1a12;
  --muted: #5a665d;
  --line: #d5ddd3;
  --accent: #145431;
  --accent-ink: #ffffff;
  color-scheme: light;
}
@media (prefers-color-scheme: dark) {
  :root {
    --page: #0f1511;
    --card: #161d18;
    --ink: #e3ebe2;
    --muted: #93a096;
    --line: #2a342c;
    --accent: #95d5a7;
    --accent-ink: #00341a;
    color-scheme: dark;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0;
  background: var(--page);
  color: var(--ink);
  font: 14px/1.45 Inter, system-ui, sans-serif;
}
header {
  position: sticky;
  top: 0;
  z-index: 1;
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 10px 20px;
  padding: 12px 24px;
  background: var(--page);
  border-bottom: 1px solid var(--line);
}
h1 { margin: 0; font-size: 16px; font-weight: 700; }
#search {
  flex: 1 1 260px;
  max-width: 420px;
  padding: 8px 12px;
  font: inherit;
  color: inherit;
  background: var(--card);
  border: 1px solid var(--line);
  border-radius: 8px;
}
#search:focus { outline: 2px solid var(--accent); outline-offset: 1px; }
.styles { display: flex; gap: 14px; color: var(--muted); }
.styles label { display: flex; align-items: center; gap: 5px; cursor: pointer; }
.styles input { accent-color: var(--accent); margin: 0; }
#count { margin-left: auto; color: var(--muted); font-variant-numeric: tabular-nums; }
#grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(124px, 1fr));
  gap: 4px;
  padding: 16px 24px 80px;
}
.icon {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 8px;
  padding: 14px 6px 10px;
  font: inherit;
  color: inherit;
  background: none;
  border: 1px solid transparent;
  border-radius: 8px;
  cursor: pointer;
}
/* Without this the display above also shows the icons that the search hides. */
.icon[hidden] { display: none; }
.icon:hover, .icon:focus-visible { background: var(--card); border-color: var(--line); outline: none; }
.icon i { font: normal 32px/1 remixicon; }
.icon span {
  font-size: 12px;
  line-height: 1.3;
  color: var(--muted);
  text-align: center;
  overflow-wrap: anywhere;
}
#empty { padding: 48px 24px; color: var(--muted); text-align: center; }
#toast {
  position: fixed;
  left: 50%;
  bottom: 24px;
  transform: translateX(-50%);
  padding: 10px 16px;
  background: var(--accent);
  color: var(--accent-ink);
  border-radius: 8px;
  font: 13px ui-monospace, monospace;
  opacity: 0;
  transition: opacity 0.15s;
  pointer-events: none;
}
#toast.shown { opacity: 1; }
</style>
</head>
<body>
<header>
  <h1>Remix Icon @RELEASE@</h1>
  <input id="search" type="search" placeholder="Search @COUNT@ icons by name" autofocus autocomplete="off" spellcheck="false">
  <div class="styles">
    <label><input type="radio" name="style" value="all" checked> All</label>
    <label><input type="radio" name="style" value="line"> Line</label>
    <label><input type="radio" name="style" value="fill"> Fill</label>
  </div>
  <span id="count"></span>
</header>
<main id="grid"></main>
<p id="empty" hidden>No icon has this name.</p>
<div id="toast" role="status"></div>
<script>
// Each icon's name and its code point in hexadecimal, sorted by name.
const ICONS = @ICONS@;

const grid = document.getElementById("grid");
const search = document.getElementById("search");
const count = document.getElementById("count");
const empty = document.getElementById("empty");
const toast = document.getElementById("toast");
let toastTimer = 0;

function copyThroughSelection(text) {
  const area = document.createElement("textarea");
  area.value = text;
  document.body.append(area);
  area.select();
  const isCopied = document.execCommand("copy");
  area.remove();
  return isCopied;
}

async function copy(text) {
  let isCopied = false;
  try {
    await navigator.clipboard.writeText(text);
    isCopied = true;
  } catch {
    // A page opened from a file has no clipboard in some browsers.
    isCopied = copyThroughSelection(text);
  }
  toast.textContent = isCopied ? "Copied " + text : "Could not copy " + text;
  toast.classList.add("shown");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove("shown"), 1800);
}

const cells = ICONS.map(([name, codePoint]) => {
  const code = 'icon("' + name + '")';
  const cell = document.createElement("button");
  cell.type = "button";
  cell.className = "icon";
  cell.title = "Copy " + code;
  const glyph = document.createElement("i");
  glyph.setAttribute("aria-hidden", "true");
  glyph.textContent = String.fromCharCode(parseInt(codePoint, 16));
  const label = document.createElement("span");
  label.textContent = name;
  cell.append(glyph, label);
  cell.addEventListener("click", () => copy(code));
  return { name, cell };
});
grid.append(...cells.map((entry) => entry.cell));

// An icon with a single drawing has neither ending, so it shows under both choices.
function isStyleShown(name, style) {
  if (style === "line") {
    return !name.endsWith("-fill");
  }
  if (style === "fill") {
    return !name.endsWith("-line");
  }
  return true;
}

function refresh() {
  const words = search.value.toLowerCase().split(/\s+/).filter(Boolean);
  const style = document.querySelector('input[name="style"]:checked').value;
  let shownCount = 0;
  for (const { name, cell } of cells) {
    const isShown = isStyleShown(name, style) && words.every((word) => name.includes(word));
    cell.hidden = !isShown;
    if (isShown) {
      shownCount += 1;
    }
  }
  count.textContent = shownCount + " shown";
  empty.hidden = shownCount > 0;
}

search.addEventListener("input", refresh);
for (const choice of document.querySelectorAll('input[name="style"]')) {
  choice.addEventListener("change", refresh);
}
// A link can carry a search: remixicon_showcase.html#chart line
search.value = decodeURIComponent(location.hash.slice(1));
refresh();
</script>
</body>
</html>
"""


def read_icons(css_path):
    """Returns the release and each icon's code point by name, or stops on a release that can't be used."""
    css = css_path.read_text(encoding="utf-8")
    release = RELEASE_LINE.search(css)
    if release is None:
        raise SystemExit(f"{css_path}: no 'Remix Icon v<release>' line")
    if LICENCE_LINE not in css.splitlines():
        raise SystemExit(
            f"{css_path}: Remix Icon {release.group(1)} is not under the Apache License 2.0, "
            "and docs/ongoing_work/assets.md keeps the font on a release that is"
        )

    code_point_of_name = {}
    for name, code_point_text in ICON_RULE.findall(css):
        code_point = int(code_point_text, 16)
        if name in code_point_of_name:
            raise SystemExit(f"{css_path}: the icon {name} is listed twice")
        if code_point not in PRIVATE_USE_AREA:
            raise SystemExit(f"{css_path}: the icon {name} is outside the private use area")
        code_point_of_name[name] = code_point

    # A rule this script can't read would leave an icon out without a word.
    rule_count = css.count(":before {")
    if len(code_point_of_name) != rule_count:
        raise SystemExit(f"{css_path}: read {len(code_point_of_name)} icons out of {rule_count} rules")
    if len(set(code_point_of_name.values())) != len(code_point_of_name):
        raise SystemExit(f"{css_path}: two icons share a code point")
    return release.group(1), code_point_of_name


def code_list(release, code_point_of_name):
    lines = [
        f"// Written by src/ui/common/list_icons.py from Remix Icon {release}, https://remixicon.com.",
        "// Copyright RemixIcon.com, released under the Apache License Version 2.0, which is in",
        "// assets/remixicon_license.txt.",
        "// Not to be edited: run the script again on the release of assets/remixicon.ttf.",
        "#pragma once",
        "#include <array>",
        "#include <string_view>",
        "",
        "struct Icon {",
        "    std::string_view name;",
        "    const char* text;  // one character, to draw with any ImGui text call",
        "};",
        "",
        "// Sorted by name.",
        "// clang-format off",
        f"inline constexpr std::array<Icon, {len(code_point_of_name)}> ALL_ICONS{{",
    ]
    for name in sorted(code_point_of_name):
        lines.append(f'    Icon{{.name = "{name}", .text = "\\u{code_point_of_name[name]:04X}"}},')
    lines += ["};", "// clang-format on", ""]
    return "\n".join(lines)


def showcase_page(release, code_point_of_name):
    icons = [[name, f"{code_point_of_name[name]:04x}"] for name in sorted(code_point_of_name)]
    return (
        SHOWCASE_PAGE.replace("@RELEASE@", release)
        .replace("@COUNT@", f"{len(icons):,}")
        .replace("@ICONS@", json.dumps(icons, separators=(",", ":")))
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--css", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--showcase", type=pathlib.Path, required=True)
    arguments = parser.parse_args()

    release, code_point_of_name = read_icons(arguments.css)
    arguments.output.write_text(code_list(release, code_point_of_name), encoding="utf-8")
    arguments.showcase.write_text(showcase_page(release, code_point_of_name), encoding="utf-8")
    for written in (arguments.output, arguments.showcase):
        print(f"{written}: {len(code_point_of_name)} icons of Remix Icon {release}")


if __name__ == "__main__":
    main()
