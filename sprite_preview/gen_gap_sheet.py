#!/usr/bin/env python3
"""Generate a sheet for the tiles the art pack cannot draw, from the gap report.

This is the loop the design note describes, made concrete: `AIRogue --gaps` writes
the worklist, this turns it into one image request, and the slicer cuts it up.
It is the "drain gap.json" step of docs/asset-generation-design.md section 7.

Why a separate script rather than another mode on gen_tile_sheet.py: the two ask
for different pictures. gen_tile_sheet.py wants SEAMLESS TOP-DOWN GROUND TEXTURES
and explicitly forbids props, because a prop baked into a floor tile repeats on
every cell. The gaps here are the opposite -- anvil, forge, workbench, a door and
a water trough are single-cell PROPS that happen to be stored as tiles. Asking for
them with the ground-texture prompt would produce seamless stone, not an anvil.

So the constraints invert:

  * full-bleed and opaque, still edge to edge (a tile must cover its cell, and the
    slicer removes alpha), but on the tile's own authored backgroundColor rather
    than as a repeatable texture;
  * NOT seamless, and asked to be non-repeating, since nothing tiles a forge;
  * no magenta gutter needed -- these cells are opaque, so there is nothing to key
    except the background between cells;
  * the palette swatch sheet is attached as a reference, so the colours come from
    the pack rather than from the model's idea of "medieval".

Usage:
  # see the prompt and layout without spending anything
  python3 sprite_preview/gen_gap_sheet.py --dry-run

  # the worklist drives it: data/gaps.json is picked up automatically when it
  # exists, so this draws exactly what `AIRogue --gaps` said the pack cannot
  python3 sprite_preview/gen_gap_sheet.py

  # a different report, in small batches, into a named sheet
  python3 sprite_preview/gen_gap_sheet.py --from-gaps other.json --only 3 \\
      --out sprite_preview/raw/gap_batch2.png
"""

import argparse
import base64
import json
import math
import os
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

MODEL = "google/gemini-2.5-flash-image"
API = "https://openrouter.ai/api/v1/chat/completions"
OUT = os.path.join(HERE, "raw", "gap_tiles_sheet.png")

# Style anchor, from the hand-curated pack rather than from earlier generated
# output. See docs/asset-generation-design.md 1.2: nothing that models style may
# read the generated layer.
REFERENCE_SPRITE = os.path.join(ROOT, "data", "sprites", "tile_stone_wall.png")

# The palette goes in as TEXT, not as the swatch sheet.
#
# It was attached as an image on the first run and the model drew the swatches --
# a row of palette squares across the top of the sheet. A reference image is
# something to imitate, and a grid of coloured squares is a perfectly imitable
# picture; "do not use colours outside it" does not stop it being copied. The
# colours are just data, so they belong in the prompt.
PALETTE_JSON = os.path.join(ROOT, "data", "palette.json")
PALETTE_SAMPLE = 48  # evenly spaced across the derived list, to keep it readable

# Where `AIRogue --gaps` writes its report, and so the default worklist.
GAPS_DEFAULT = os.path.join(ROOT, "data", "gaps.json")

# Hand-written prompts for the subjects the gap report found. A written description
# beats anything derivable from a name, so these win when the name matches.
#
# They are an OVERRIDE, not a gate. A gap tile with no entry here still gets
# generated, from a derived prompt. An earlier version skipped unknown names, which
# defeated the point: the report could list a gap that nothing would ever fill.
KNOWN_SUBJECTS = {
    "anvil":
        "an iron blacksmith's anvil on a short wooden stump, seen from directly "
        "above: dark grey iron, a smooth flat top, a small horn",
    "door":
        "a closed wooden door seen from directly above: vertical brown planks with "
        "two dark iron bands and small iron studs",
    "forge":
        "a blacksmith's forge seen from directly above: a squat stone hearth with a "
        "dark mouth full of glowing orange coals and a little white ash",
    "water trough":
        "a wooden water trough seen from directly above: a rectangular plank trough "
        "holding still pale blue water with a faint lighter ripple",
    "workbench":
        "a wooden workbench seen from directly above: a thick plank top with a "
        "hammer, a few nails and a small saw laid on it",
}


def describe_subject(name, definition):
    """A prompt for one subject: the hand-written one, else one derived from data.

    The derived form is deliberately literal. Inventing detail for an unfamiliar
    name is how a generated sprite ends up depicting the wrong object, and a plain
    prompt that gets reviewed beats a fluent one that gets trusted.
    """
    if name in KNOWN_SUBJECTS:
        return KNOWN_SUBJECTS[name], True
    description = str(definition.get("description", "")).strip()
    if description:
        return f"{description} Seen from directly above as one object", False
    return f"a {name}, seen from directly above as a single object on the ground", False


def describe_colour(rgb):
    """A rough name for an RGB triple: 'dark brown', 'very dark blue'.

    Approximate on purpose. It gives the model a target in words; the hex list
    pins the exact colours, so precision here would be wasted.
    """
    r, g, b = int(rgb[0]), int(rgb[1]), int(rgb[2])
    high, low = max(r, g, b), min(r, g, b)
    if high - low < 24:
        hue = "grey"
    elif r >= g >= b:
        hue = "brown" if g > r * 0.5 and b > r * 0.35 else "orange"
    elif b >= g >= r:
        hue = "blue"
    elif g >= r and g >= b:
        hue = "green"
    else:
        hue = "purple"
    level = high / 255.0
    if level < 0.25:
        return "very dark " + hue
    if level < 0.5:
        return "dark " + hue
    if level > 0.85:
        return "pale " + hue
    return hue


def load_tile_definitions(directory=None):
    """Every data/tiles/*.json, keyed by the tile's authored name."""
    directory = directory or os.path.join(ROOT, "data", "tiles")
    definitions = {}
    for entry in sorted(os.listdir(directory)):
        if not entry.endswith(".json"):
            continue
        with open(os.path.join(directory, entry)) as handle:
            definition = json.load(handle)
        if definition.get("name"):
            definitions[definition["name"]] = definition
    return definitions


def api_key():
    with open(os.path.join(ROOT, "model.json")) as handle:
        return json.load(handle)["apiKey"]


def extract_image(payload):
    found = []

    def walk(node):
        if isinstance(node, dict):
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)
        elif isinstance(node, str) and node.startswith("data:image/"):
            found.append(node)

    walk(payload)
    return found[0] if found else None


def resolve_worklist(from_gaps):
    """The subjects to draw, as dicts carrying their name, prompt and tile data.

    Pairs rather than bare names for two reasons: the prompt needs each tile's
    authored tone, and an unfamiliar name must still produce a prompt. Skipping
    unknown names is the bug this replaced -- the report could list a gap that
    nothing would ever fill.
    """
    definitions = load_tile_definitions()
    if from_gaps:
        with open(from_gaps) as handle:
            report = json.load(handle)
        names = sorted({entry["name"] for entry in report.get("coverage", [])
                        if entry.get("kind") == "tile"})
        if not names:
            sys.exit(f"{from_gaps} lists no gap tiles (kind 'tile') to draw")
    else:
        names = sorted(KNOWN_SUBJECTS)

    subjects = []
    derived = []
    for name in names:
        definition = definitions.get(name, {})
        description, handwritten = describe_subject(name, definition)
        if not handwritten:
            derived.append(name)
        subjects.append({"name": name, "description": description,
                         "definition": definition})

    if derived:
        # Surfaced rather than buried. A derived prompt is the one place this
        # script guesses, and a guess is exactly what review is for.
        print(f"note: {len(derived)} subject(s) have no hand-written prompt and use a "
              f"derived one -- check these on the sheet:")
        print(f"      {', '.join(derived)}")
    return subjects


def palette_text():
    """The derived pack palette as hex codes, or "" when there is none."""
    if not os.path.exists(PALETTE_JSON):
        return ""
    with open(PALETTE_JSON) as handle:
        colors = [tuple(c) for c in json.load(handle)["colors"]]
    if not colors:
        return ""
    # Evenly spaced rather than the first N: the list is sorted by brightness, so
    # taking a prefix would hand the model only the darkest half of the pack.
    step = max(1, len(colors) // PALETTE_SAMPLE)
    sample = colors[::step]
    return ", ".join(f"#{r:02X}{g:02X}{b:02X}" for r, g, b in sample)


def build_prompt(subjects, cols):
    rows = math.ceil(len(subjects) / cols)

    # Each subject's tone comes from its own tile definition, so the cell keeps the
    # background the ASCII tile already had instead of a hand-copied phrase that
    # drifts from the data.
    parts = []
    for i, subject in enumerate(subjects):
        background = subject["definition"].get("backgroundColor")
        if background and len(background) >= 3:
            tone = (f" a flat {describe_colour(background)} "
                    f"(#{background[0]:02X}{background[1]:02X}{background[2]:02X}) "
                    f"background that fills the whole cell")
        else:
            tone = (" a flat background colour taken from the palette below, "
                    "filling the whole cell")
        parts.append(f"{i + 1}) {subject['description']} (centre it on{tone})")
    order = "; ".join(parts)

    empties = cols * rows - len(subjects)
    palette = palette_text()
    palette_clause = (
        f"The pack's colour palette is exactly these colours, sampled from the "
        f"pack itself: {palette}. Draw only with colours from that list. "
        if palette else "")
    return (
        f"A 16-bit pixel art sprite sheet: a grid of {cols} columns by {rows} rows "
        f"of equal square cells. Fill only the first {len(subjects)} cells, in "
        f"reading order, left to right then top to bottom, with these objects in "
        f"this exact order: {order}. "
        # The first generated sheet filled the unused cells with a flat dark
        # background colour, which is not magenta and therefore survived keying --
        # so those cells came back as full-cell blobs and the slicer counted seven
        # subjects where five were asked for. Magenta is the only value that keys.
        f"Leave the remaining {empties} cell{'' if empties == 1 else 's'} COMPLETELY "
        f"EMPTY: fill {'it' if empties == 1 else 'them'} edge to edge with pure "
        f"magenta (#FF00FF) exactly like the gaps between the cells, with no object "
        f"and no dark or coloured background fill of any kind. "
        "Every cell is a TOP-DOWN view seen from directly above, flat and evenly "
        "lit: no perspective, no horizon, no side view, no cast shadow outside the "
        "object and no lighting gradient across the cell. "
        "Each object must be FULL-BLEED: it fills its whole square cell edge to "
        "edge with its background colour, with no transparency anywhere, and no "
        "border, outline, frame, vignette, panel or grid line around the cell. "
        "These cells are drawn flat on the ground, so they do not tile and must not "
        "be asked to: each is one object in one cell, not a repeating texture. "
        "Do not draw any floor, grass, dirt or ground texture, no wall, no "
        "character and no creature. No text, labels, numbers or grid lines. "
        "STYLE: chunky retro pixel art with a limited palette, matching the look of "
        "the attached sprite -- the same flat shading without outlines, the same "
        "level of detail, the same limited palette. "
        + palette_clause +
        "The attached sprite is a style reference only: do not draw stone wall, and "
        "do not draw any colour swatches, palette strip or sample squares. "
        f"Leave a clear gap of flat background colour between neighbouring cells so "
        f"they are cleanly separated, and make that gap one flat, uniform, fully "
        f"saturated magenta colour (#FF00FF)."
    )


def display_path(path):
    """Repo-relative when it is inside the repo, absolute otherwise.

    os.path.relpath alone turns a path outside the tree into a run of '../', which
    is worse than useless in a log line.
    """
    absolute = os.path.abspath(path)
    root = os.path.abspath(ROOT) + os.sep
    return absolute[len(root):] if absolute.startswith(root) else absolute


def parse_args():
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--from-gaps", default="",
                        help="a gaps.json written by `AIRogue --gaps`, whose coverage "
                             "misses become the subject list. Defaults to "
                             "data/gaps.json when that file exists")
    parser.add_argument("--out", default=OUT, help="where the sheet is written")
    parser.add_argument("--cols", type=int, default=3,
                        help="cells per row (default 3). The model has been observed "
                             "returning a different grid than the one asked for, so "
                             "treat the layout as a hint and read the index sheet")
    parser.add_argument("--only", type=int, default=0,
                        help="draw at most this many subjects in one request. The "
                             "model dropped a cell from a five-cell sheet, so small "
                             "batches are safer than one big one")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the prompt and make no request")
    return parser.parse_args()


def main():
    args = parse_args()
    global OUT
    OUT = args.out

    # A worklist-driven default, because that is the point of the script: if a gap
    # report exists, it is the current truth about what the pack cannot draw.
    from_gaps = args.from_gaps
    if not from_gaps and os.path.exists(GAPS_DEFAULT):
        from_gaps = GAPS_DEFAULT

    worklist = resolve_worklist(from_gaps)
    subjects = worklist if args.only <= 0 else worklist[: args.only]
    remaining = len(worklist) - len(subjects)

    cols = max(1, args.cols)
    rows = math.ceil(len(subjects) / cols)
    style = build_prompt(subjects, cols)

    print(f"model      : {MODEL}")
    print(f"worklist   : {display_path(from_gaps) if from_gaps else '(hand-written table)'}"
          f" -- {len(worklist)} gap tile(s)")
    print(f"subjects   : {len(subjects)} -> "
          f"{', '.join(s['name'] for s in subjects)}")
    if remaining:
        print(f"             {remaining} more for a later sheet (use --only/--out)")
    print(f"layout     : asked for {cols}x{rows} with {cols * rows - len(subjects)} "
          f"empty cell(s) -- a hint, not a contract")
    print(f"reference  : {os.path.relpath(REFERENCE_SPRITE, ROOT)}"
          + ("" if os.path.exists(REFERENCE_SPRITE) else "  (MISSING)"))
    print(f"palette    : {len(palette_text().split(', ')) if palette_text() else 0} "
          f"colours in the prompt as hex"
          + ("" if os.path.exists(PALETTE_JSON) else "  (no data/palette.json)"))
    print(f"output     : {display_path(OUT)}")
    print()
    print(style)
    print()

    if args.dry_run:
        print("dry run: no request made")
        return

    content = [{"type": "text", "text": style}]
    for path, label in ((REFERENCE_SPRITE, "sprite"),):
        if not os.path.exists(path):
            continue
        with open(path, "rb") as handle:
            encoded = base64.b64encode(handle.read()).decode()
        content.append({"type": "image_url",
                        "image_url": {"url": "data:image/png;base64," + encoded}})
        print(f"attached {label}: {os.path.relpath(path, ROOT)}")

    body = {
        "model": MODEL,
        "modalities": ["image", "text"],
        "messages": [{"role": "user", "content": content}],
    }
    request = urllib.request.Request(
        API,
        data=json.dumps(body).encode(),
        headers={
            "Authorization": "Bearer " + api_key(),
            "Content-Type": "application/json",
            "HTTP-Referer": "https://localhost/airogue-sprite-preview",
            "X-Title": "AIRogue gap tile sheet",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=300) as response:
            payload = json.load(response)
    except urllib.error.HTTPError as error:
        sys.exit(f"HTTP {error.code}: {error.read().decode(errors='replace')[:800]}")

    data_url = extract_image(payload)
    if data_url is None:
        message = (payload.get("choices") or [{}])[0].get("message") or {}
        sys.exit(f"no image in response: {json.dumps(message)[:800]}")

    _, encoded = data_url.split(",", 1)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as handle:
        handle.write(base64.b64decode(encoded))
    with open(os.path.join(HERE, "raw", "gap_tiles_sheet.prompt.txt"), "w") as handle:
        handle.write(style + "\n")

    usage = payload.get("usage") or {}
    print(f"wrote {os.path.relpath(OUT, ROOT)} ({os.path.getsize(OUT)} bytes)")
    print(f"cost: {usage.get('cost')}  prompt_tokens: {usage.get('prompt_tokens')}  "
          f"image_tokens: {(usage.get('completion_tokens_details') or {}).get('image_tokens')}")


if __name__ == "__main__":
    main()
