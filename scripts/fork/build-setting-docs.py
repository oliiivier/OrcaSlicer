#!/usr/bin/env python3
"""Fork-only: bundles the setting documentation for the MCP server.

Reads the setting -> wiki page links of src/slic3r/GUI/Tab.cpp and the Markdown
pages of doc/, and writes resources/ai_assistant/setting_docs.json:

  {
    "pages":    { "<page>": { "title", "path", "sections": [{ "anchor", "title", "level", "text" }] } },
    "settings": { "<setting key>": "<page>[#<anchor>]" }
  }

Sections are flat (own text only, in page order); the server joins a section
with its subsections. Images are dropped. Run from anywhere; see FORK.md.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TAB_CPP = ROOT / "src" / "slic3r" / "GUI" / "Tab.cpp"
DOC_DIR = ROOT / "doc"
OUTPUT = ROOT / "resources" / "ai_assistant" / "setting_docs.json"

# Developer notes are not user documentation.
EXCLUDED_DIRS = {"developer-reference"}

LINK_RE = re.compile(r'append_single_option_line\(\s*"([a-z0-9_]+)"\s*,\s*"([^"]+)"')
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
IMAGE_RE = re.compile(r"!\[[^\]]*\]\([^)]*\)")
HTML_IMG_RE = re.compile(r"<img\b[^>]*>", re.IGNORECASE)
HTML_TAG_RE = re.compile(r"</?(?:div|p|br|center|picture|source|span)\b[^>]*>", re.IGNORECASE)


def slug(text):
    """Heading anchor, same rule as .github/workflows/validate-documentation.yml."""
    text = re.sub(r"[^a-z0-9\s-]", "", text.lower().strip())
    return re.sub(r"^-+|-+$", "", text.replace(" ", "-"))


def clean(lines):
    text = "\n".join(lines)
    text = IMAGE_RE.sub("", text)
    text = HTML_IMG_RE.sub("", text)
    text = HTML_TAG_RE.sub("", text)
    text = re.sub(r"[ \t]+\n", "\n", text)
    return re.sub(r"\n{3,}", "\n\n", text).strip()


def parse_page(path):
    sections = []
    current = {"anchor": "", "title": path.stem, "level": 0, "lines": []}
    in_code = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("```"):
            in_code = not in_code
        match = None if in_code else HEADING_RE.match(line)
        if match:
            sections.append(current)
            title = match.group(2)
            current = {"anchor": slug(title), "title": title, "level": len(match.group(1)), "lines": []}
        else:
            current["lines"].append(line)
    sections.append(current)

    out = []
    for s in sections:
        text = clean(s["lines"])
        if text or s["level"] > 0:
            out.append({"anchor": s["anchor"], "title": s["title"], "level": s["level"], "text": text})
    title = next((s["title"] for s in out if s["level"] == 1), path.stem)
    return {"title": title, "path": path.relative_to(DOC_DIR).as_posix(), "sections": out}


def main():
    pages = {}
    by_lower_name = {}
    for path in sorted(DOC_DIR.rglob("*.md")):
        if EXCLUDED_DIRS.intersection(path.relative_to(DOC_DIR).parts):
            continue
        pages[path.stem] = parse_page(path)
        by_lower_name[path.stem.lower()] = path.stem

    settings = {}
    warnings = []
    for key, target in LINK_RE.findall(TAB_CPP.read_text(encoding="utf-8")):
        if key in settings:
            continue  # the same setting shown in several tabs: keep the first link
        page, _, anchor = target.partition("#")
        name = by_lower_name.get(page.lower())
        if name is None:
            warnings.append(f"{key}: no page '{page}'")
            continue
        anchors = {s["anchor"] for s in pages[name]["sections"]}
        if anchor and slug(anchor) not in anchors:
            warnings.append(f"{key}: no section '#{anchor}' in '{name}', linking the page")
            anchor = ""
        settings[key] = name + ("#" + slug(anchor) if anchor else "")

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    data = {"pages": pages, "settings": dict(sorted(settings.items()))}
    OUTPUT.write_text(json.dumps(data, ensure_ascii=False, indent=1, sort_keys=False) + "\n", encoding="utf-8")

    for w in warnings:
        print("warning: " + w, file=sys.stderr)
    print(f"{OUTPUT.relative_to(ROOT)}: {len(settings)} settings, {len(pages)} pages")


if __name__ == "__main__":
    main()
