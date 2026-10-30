#!/usr/bin/env python3
"""Generate sprites for tiles the art pack cannot draw -- one request, many tiles.

This is the SOURCE + CUT + PALETTE stages of docs/asset-generation-design.md §7,
made concrete for agent-created tiles, and the generator half of
docs/tile-art-wiring-design.md §1. Its output is what `AIRogue --tile-art` places
and hot-swaps; it writes into the *generated layer*, never the art pack, because
a generated image may not shadow hand-curated art (§1.2).

Why it does not live in the game binary
---------------------------------------
ImageMagick and Pillow are not in the default PATH on this machine, the request is
a network round trip of seconds to tens of seconds, and none of that belongs in a
process whose frame budget is 16ms. So: an external tool the owner runs, whose
finished PNG the game places. The seam is the file.

COST -- read this before running it
-----------------------------------
Measured for this model (docs/asset-generation-design.md §6): **~1290 image tokens
per request regardless of how many cells it fills**. So one request per tile is
roughly 19x the per-asset cost of one batched sheet. Everything here is built to
spend as few requests as possible:

  * **Batching is the default.** Pass every name you have; they go in ONE request.
  * `--dry-run` builds the prompt, prints it, and stops. Costs nothing. Use it
    first, always.
  * `--sheet` re-slices a sheet already on disk. A re-cut, a different inset, a
    palette change: all free, because the expensive part is the pixels, not the
    slicing (this is §11.2's "keep the raw renders, always").
  * The request is the only thing that costs money, and the script says how much
    it spent afterwards.

Usage (Pillow and ImageMagick are needed for the slice step):

  # free: see the prompt, the cell layout and the exact output paths
  nix-shell -p python3Packages.pillow imagemagick --run \\
      "python3 tools/gen_tile_art.py --dry-run 'oak bed' 'stone hearth'"

  # one request for all of them
  nix-shell -p python3Packages.pillow imagemagick --run \\
      "python3 tools/gen_tile_art.py 'oak bed' 'stone hearth'"

  # free: re-cut a sheet that was already paid for
  nix-shell -p python3Packages.pillow imagemagick --run \\
      "python3 tools/gen_tile_art.py --sheet work/tile_art_sheet.png 'oak bed'"
"""

import argparse
import base64
import json
import math
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The same image model the gap-sheet pipeline uses, because that path works and
# has been measured. gen_tile.py's local diffusers path is the other backend and
# is currently blocked on the host CUDA fault (see docs/tile-art-wiring-design.md
# §5 and ~/nixos/comfyui-docker.sh).
MODEL = "google/gemini-2.5-flash-image"
API = "https://openrouter.ai/api/v1/chat/completions"

# Where the sheet and the per-tile cut source go. Under the generated layer's
# sibling `work/` directory rather than inside the layer itself: the layer holds
# what the game draws, and a 1024px sheet is not that.
WORK = os.path.join(ROOT, "data", "generated", "work")

# The style reference. One sprite, kept small: reference images cost tokens too, so
# this is a single 32x32 hand-curated pack sprite rather than a contact sheet.
REFERENCE_SPRITE = os.path.join(ROOT, "data", "sprites", "tile_stone_wall.png")

# The palette. This is the *art pack's* derived palette -- the thing generated art
# is harmonised to -- and it is read as a list of colours, not attached as the
# swatch image. See gen_gap_sheet.py's note: an attached swatch sheet gets drawn.
PALETTE_JSON = os.path.join(ROOT, "data", "palette.json")

# How many palette colours go into the prompt text. Evenly spaced rather than the
# first N, because the list is sorted by brightness and a prefix would hand the
# model only the dark half of the pack.
PALETTE_SAMPLE = 48

TILE = 32

# Pixels removed from every edge of a cell, expressed in the *final* tile.
#
# This was a fixed 8 source pixels, which is 0.25 of a final pixel at the 1024 -> 32
# reduction a one-cell sheet produces: it removed nothing. The model drew a thick
# rim around its tile (the gutter colour, the full width of the frame), the inset
# could not touch it, and the rim became a red mesh border on the sprite.
#
# Stating it in final pixels and scaling by the reduction makes it mean the same
# thing at any cell size, which is why the value is now resolution-independent.
INSET_TILE_PIXELS = 2

# How close a pixel must be to the cell's edge colour to count as gutter, as a
# fraction of the channel range. Generous on purpose: the model rarely emits the
# exact colour it was asked for -- the real sheet's gutter came back as
# srgb(205,50,141) where pure magenta is (255,0,255), roughly 50 levels off on every
# channel, which no sane tolerance around #FF00FF would have matched.
GUTTER_TOLERANCE = 0.22

# The furthest a measured frame may reach inwards, as a fraction of the cell's width.
#
# A runaway guard, not a judgement about how thick a frame may be: the walk measures
# the real boundary and the cap only stops it consuming most of the cell if the gutter
# colour happens to match the art and the walk never terminates.
#
# It was 6%, which silently truncated the measurement -- the model drew a frame 145px
# thick into a 1024px cell (14%), so every walk returned 61, the rim was never fully
# removed, and the leftover gutter became the red mesh border on the sprite. Sweeping
# the sheet: a 128px inset still left 98 red pixels in the final tile, and 160px left
# exactly 0 -- consistent with the 145px boundary the walk reports plus the
# anti-aliased edge and the final margin.
MAX_FRAME_FRACTION = 0.25


def display_path(path):
    """Repo-relative when inside the repo, absolute otherwise."""
    absolute = os.path.abspath(path)
    root = os.path.abspath(ROOT) + os.sep
    return absolute[len(root):] if absolute.startswith(root) else absolute


# ---------------------------------------------------------------------------
# Reading the world's vocabulary
# ---------------------------------------------------------------------------

def load_tile_definitions():
    """Every tile definition the engine would load: catalog then overlay.

    Both directories, because an agent-created tile lives in the overlay and is
    exactly what this tool exists to draw. The overlay wins on a name collision,
    matching TileRegistry::AddRuntime's replace rule.
    """
    definitions = {}
    settings = {}
    settings_path = os.path.join(ROOT, "settings.json")
    if os.path.exists(settings_path):
        with open(settings_path) as handle:
            settings = json.load(handle)
    content = settings.get("content") or {}
    directories = [
        content.get("tiles") or os.path.join(ROOT, "data", "tiles"),
        content.get("generatedTiles") or os.path.join(ROOT, "data", "generated", "tiles"),
    ]
    for directory in directories:
        if not os.path.isabs(directory):
            directory = os.path.join(ROOT, directory)
        if not os.path.isdir(directory):
            continue
        for entry in sorted(os.listdir(directory)):
            if not entry.endswith(".json"):
                continue
            try:
                with open(os.path.join(directory, entry)) as handle:
                    definition = json.load(handle)
            except (OSError, ValueError) as error:
                print(f"warning: skipping {entry}: {error}", file=sys.stderr)
                continue
            if definition.get("name"):
                definitions[definition["name"]] = definition
    return definitions


def slugify(name):
    """The same rule as TileSpriteId (include/SpritePaths.hpp), so the file this
    writes is the file the engine looks for. Duplicated because the alternative is
    a C++/Python interface for one string transform; if it ever changes, the
    tiledesigner/tile-art mismatch will show up as "no sprite" and this comment is
    where to look."""
    slug = "tile_"
    pending = False
    for raw in name:
        if raw.isalnum():
            if pending and len(slug) > 5:
                slug += "_"
            pending = False
            slug += raw.lower()
        else:
            pending = True
    return slug


def load_palette():
    """The derived pack palette as RGB tuples, or [] when there is none."""
    if not os.path.exists(PALETTE_JSON):
        return []
    with open(PALETTE_JSON) as handle:
        payload = json.load(handle)
    return [tuple(color) for color in payload.get("colors", [])]


def palette_text(palette):
    if not palette:
        return ""
    step = max(1, len(palette) // PALETTE_SAMPLE)
    sample = palette[::step]
    return ", ".join(f"#{r:02X}{g:02X}{b:02X}" for r, g, b in sample)


def describe_colour(rgb):
    """A name for an RGB triple, from the nearest entry in a small table.

    A name rather than the hex alone because the prompt reads as language: "the
    same dark brown as the dirt" is something an image model can act on, whereas
    "#3C2819" is a number it has to interpret. The hex goes in beside it anyway,
    for the cases where the table has no good word.
    """
    r, g, b = (list(rgb) + [0, 0, 0])[:3]
    named = {
        "black": (0, 0, 0), "dark grey": (60, 60, 60), "grey": (128, 128, 128),
        "white": (240, 240, 240), "dark brown": (60, 40, 25), "brown": (120, 80, 45),
        "dark green": (30, 70, 35), "green": (90, 160, 80), "dark blue": (20, 35, 75),
        "blue": (80, 130, 200), "dark red": (70, 25, 25), "red": (170, 50, 45),
        "dark purple": (60, 40, 80), "purple": (140, 110, 180), "dark tan": (110, 90, 60),
        "tan": (200, 180, 140), "dark magenta": (90, 24, 24), "crimson": (170, 40, 40),
    }
    return min(
        named,
        key=lambda word: (r - named[word][0]) ** 2 + (g - named[word][1]) ** 2
        + (b - named[word][2]) ** 2)


def colour_of(definition, key, fallback):
    """A definition's colour as three ints, with the fallback for anything odd.

    The catalog is hand-editable and a model writes JSON, so a colour is not
    guaranteed to be the three-to-four number array the schema asks for. A
    malformed one must not stop a paid-for prompt from being built: this is the
    point where bad data would otherwise turn into a traceback, and the run would
    have already cost nothing but the user's time.
    """
    value = definition.get(key)
    if isinstance(value, list) and len(value) >= 3 and all(
            isinstance(component, int) for component in value[:3]):
        return value[:3]
    return list(fallback)



# Tiles that are a *surface* rather than a *thing standing on one*.
#
# This distinction has to be made somewhere, because the two want opposite prompts
# and "seamless/tiles well" applied to an anvil produces a grid of anvils -- which
# is what the first generated sheet came back as. A surface wants "tile this edge to
# edge"; a prop wants "exactly one, centred".
#
# **This used to be a word list, and the word list was a guess that had to go.** It scored
# a name and description against SURFACE_WORDS ("wall", "stone", "roof", ...), which was
# reasonable while every sprite was a tile and became wrong the moment objects arrived: an
# object called "stone wall" or "bell tower" contains a surface word and would have been
# asked for as a full-bleed texture, while a tile whose name happened to miss every word
# would have been asked for as a centred object. It was also invisible from the output --
# the sheet comes back plausible either way.
#
# The kind is now carried explicitly: the worklist names it per cell, because the engine
# knows it for certain (a definition lives in exactly one registry) and the generator does
# not. `--tile-art` and a hand-written worklist still work, defaulting to "tile".


def is_surface(kind, name, definition):
    """Whether this cell is drawn as a full-bleed surface rather than a centred object.

    The kind decides it. The name/description heuristics survive only for the flat
    `tiles` spelling of a worklist, where no kind was recorded -- and they are confined to
    that fallback so a worklist that *does* say the kind can never be overridden by a word
    in a name.
    """
    if kind == "tile":
        return True
    if kind == "object":
        return False
    # Unknown kind: the old heuristic, kept for a worklist written by an older build.
    lowered = name.lower()
    if any(word in lowered for word in LEGACY_SURFACE_WORDS):
        return True
    description = (definition.get("description") or "").lower()
    return any(word in description for word in ("surface", "floor", "ground"))


LEGACY_SURFACE_WORDS = (
    "floor", "ground", "grass", "dirt", "earth", "soil", "path", "road",
    "water", "carpet", "rug", "sand", "gravel", "stone", "tile", "flagstone",
    "plank", "board", "moss", "snow", "ice", "mud", "ash", "cobble", "brick",
    "wall", "ceiling", "roof", "paving", "concrete", "marble", "turf",
)


def describe_tile(kind, name, definition):
    """One cell's description, from the definition rather than from the name alone.

    The subject's own colours are the anchor: they are what the ASCII rendering already
    looks like, so the sprite keeps the authored tone instead of a phrase a human typed once
    and then drifted from the data.

    `kind` decides the *shape* of the prompt, not just the wording: a tile is a surface that
    covers its cell edge to edge and repeats, an object is one complete thing centred in
    it. That used to be guessed from the name (see `is_surface`), which was wrong for any
    object whose name contained a ground word.
    """
    foreground = colour_of(definition, "characterColor", (255, 255, 255))
    background = colour_of(definition, "backgroundColor", (128, 128, 128))
    description = definition.get("description")
    surface = is_surface(kind, name, definition)

    if description:
        subject = description.rstrip(".")
    elif surface:
        subject = f"a seamless {name} surface"
    else:
        # Not "a view of X": the sheet is a grid of cells, and a noun phrase
        # describing one object is what keeps the model from drawing the object
        # many times to fill the cell.
        subject = f"a single {name}"

    shape = ("a full-bleed SURFACE TEXTURE covering the whole cell" if surface
             else "ONE SINGLE complete object, centred in the cell")
    return (
        f"{subject}, drawn as {shape}. Its dominant colour is "
        f"{describe_colour(background)} "
        f"(#{background[0]:02X}{background[1]:02X}{background[2]:02X}) with "
        f"details in {describe_colour(foreground)} "
        f"(#{foreground[0]:02X}{foreground[1]:02X}{foreground[2]:02X}). "
        f"ONE single \"{name}\" and nothing else -- "
        + ("the texture covers the cell edge to edge and repeats across a floor."
           if surface else
           "do NOT repeat it, do NOT draw a row, grid, pattern or collection of "
           "them, and do NOT draw a border or frame around the cell.")
    )


def build_prompt(cells, definitions, palette, cols):
    """The whole sheet prompt.

    `cells` is a list of {"name": str, "kind": "tile"|"object"|""} in reading order -- the
    kind per cell rather than per sheet, because a set may mix them and each cell's
    description depends on its own kind.
    """
    names = [cell["name"] for cell in cells]
    rows = math.ceil(len(names) / cols)
    described = []
    for index, cell in enumerate(cells):
        described.append(
            f"{index + 1}) {describe_tile(cell['kind'], cell['name'], definitions[cell['name']])}"
        )
    order = "; ".join(described)
    empties = cols * rows - len(names)
    surfaces = [cell["name"] for cell in cells
                if is_surface(cell["kind"], cell["name"], definitions[cell["name"]])]

    # No palette list goes into the prompt, and that is a correction rather than an
    # omission.
    #
    # It used to name every derived colour as a hex list, on the theory that "draw
    # only with these" would keep the output on-palette. What it actually did was
    # hand the model a *palette*, which it then drew: the first real in-game
    # generation came back as a swatch sheet -- rows of coloured squares with hex
    # labels around a puddle, and a magenta frame from the gutter colour. Repeating
    # "do not draw any colour swatches" in the same prompt did not help, which is the
    # tell: a list of colours is a picture of a list of colours, and an instruction
    # cannot make it not be one.
    #
    # The harmonisation this was trying to buy is already the next stage's job, and
    # `-remap` does it deterministically and without asking the model for anything:
    # the palette lock runs before the downscale precisely so the output lands on the
    # pack's colours. So the prompt keeps only the tile's own two colours (which come
    # from its definition and describe the subject, not a list) and says nothing at
    # all about the wider palette.
    palette_clause = ""
    if palette:
        palette_clause = (
            "Use a small number of flat colours, in the muted earthy range of the "
            "attached reference, with no gradients and no anti-aliasing. ")
    _ = palette_text  # kept: the swatch text is useful when debugging a prompt by hand

    empty_clause = ""
    if empties > 0:
        empty_clause = (
            f"Leave the remaining {empties} "
            f"cell{'' if empties == 1 else 's'} COMPLETELY EMPTY: fill "
            f"{'it' if empties == 1 else 'them'} edge to edge with pure magenta "
            f"(#FF00FF) exactly like the gaps between cells, with no object and no "
            f"dark or coloured background fill of any kind. ")

    # One cell = one thing. Said as its own constraint because the individual cell
    # descriptions are long and this is the rule the first sheet broke.
    if len(surfaces) == len(names):
        subject_word = "texture"
    elif not surfaces:
        subject_word = "object"
    else:
        subject_word = "subject (texture or object, per cell)"
    count_clause = (
        f"EXACTLY ONE subject per cell: each filled cell contains a single "
        f"{subject_word} and nothing else. Never draw the same subject more than "
        f"once, and never draw a repeated motif, a row, or a grid of them inside "
        f"one cell. ")

    # The cell's background. A tile's fills the cell because the tile *is* its surface. An
    # object's is a flat magenta key that the cut stage removes, so it must be uniform,
    # reach the edges, and the object must not touch them -- an object bleeding off the cell
    # edge gets cut in half when the key is removed.
    if surfaces and len(surfaces) == len(names):
        background_clause = (
            "The subject's background fills its whole square cell edge to edge, with no "
            "transparency. ")
    elif not surfaces:
        background_clause = (
            "The cell's background is one flat, uniform, fully saturated magenta colour "
            "(#FF00FF) reaching every edge of the cell. The subject sits centred on it and "
            "must NOT touch or cross any edge of the cell, and the magenta must not appear "
            "inside the subject, so the background can be removed cleanly afterwards. No "
            "ground, floor, table or shadow behind the subject. ")
    else:
        background_clause = (
            "A surface cell's background fills it edge to edge with no transparency. An "
            "object cell's background is one flat, uniform, fully saturated magenta "
            "(#FF00FF) reaching every edge, with the object centred and NOT touching any "
            "edge, so it can be removed cleanly; no ground or shadow behind it. ")

    # What a cell *is*, which differs by kind. A tile is ground and must be drawn as
    # ground -- not as a picture of something standing on a floor. An object is the
    # opposite: one thing, floating in its own cell, with no ground plane implied.
    if surfaces and len(surfaces) == len(names):
        subject_framing_clause = (
            "The cell is a piece of ground, not a picture of an object on a shelf. ")
    elif not surfaces:
        subject_framing_clause = (
            "The cell holds one object and nothing else: no floor, no table, no shelf and "
            "no ground plane under it, and the object is not standing on anything. ")
    else:
        subject_framing_clause = (
            "A cell that is a surface is ground, not a picture of something on a floor; a "
            "cell that is an object holds that object alone, with no floor, shelf or "
            "ground plane under it. ")

    # The gutter is what separates neighbouring cells, so it is only asked for when
    # there *are* neighbouring cells. On a one-cell sheet it actively hurt: the cell
    # description asks for a full-bleed surface and this clause asked for a magenta gap
    # around it, so the model drew the surface edge to edge and left no gutter at all --
    # and the cut stage, which assumed the cell's edge colour was the gutter, then ate
    # 73% of the tile. Asking for what can be honoured is also asking for a state the
    # cut stage can trust.
    gutter_clause = ""
    if cols > 1 or rows > 1:
        gutter_clause = (
            f"Leave a clear gap of flat background colour between neighbouring cells "
            f"so they are cleanly separated, and make that gap one flat, uniform, fully "
            f"saturated magenta colour (#FF00FF)."
        )

    return (
        f"A pixel art sprite sheet: a grid of {cols} columns by {rows} rows of equal "
        f"square cells. Fill only the first {len(names)} "
        f"cell{'' if len(names) == 1 else 's'}, in reading order, left to right then "
        f"top to bottom, each with ONE subject, in this exact order: {order} "
        + count_clause + empty_clause +
        "Every cell is drawn as seen from DIRECTLY ABOVE, flat and evenly lit: no "
        "perspective, no horizon, no side view, no cast shadow, no lighting gradient "
        "across the cell. "
        # The background instruction is the one that *must* differ by kind, and it was
        # global: "the background fills the cell, with no transparency" is exactly right for
        # a tile and exactly wrong for an object, which needs its background removable or
        # it covers the ground it stands on. Told the global sentence, the model drew every
        # object on a filled rectangle -- measured, a generated object sprite came back
        # 1024/1024 opaque with a solid grey corner -- and the object blotted out the tiles.
        + background_clause +
        "No border, outline, frame, vignette, panel, bevel or grid line around the cell "
        "or around the subject, and no drop shadow. "
        # The "what this cell is" sentence depends on the kind, and it is the one line that
        # was actively wrong for objects: told a cell is "a piece of ground", a model
        # drawing a crate puts it on a floor, which is exactly the shelf-picture framing
        # this sentence exists to forbid. A mixed sheet gets the neutral spelling.
        + subject_framing_clause +
        "Do not draw any character, creature or text, and no labels, numbers or grid "
        "lines. "
        "STYLE: chunky retro pixel art with a limited palette, matching the look of "
        "the attached sprite -- the same flat shading without outlines, the same "
        "level of detail, the same limited palette. "
        + palette_clause +
        "The attached sprite is a STYLE reference only: do not draw it, and do not "
        "draw any colour swatches, palette strip or sample squares. "
        + gutter_clause
    )


# ---------------------------------------------------------------------------
# The request -- the only thing here that costs money
# ---------------------------------------------------------------------------

def api_key():
    with open(os.path.join(ROOT, "model.json")) as handle:
        key = json.load(handle).get("apiKey")
    if not key:
        sys.exit("no \"apiKey\" in model.json")
    return key


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


def generate_sheet(prompt, destination):
    """One request. Returns the usage dict the API reported."""
    content = [{"type": "text", "text": prompt}]
    if os.path.exists(REFERENCE_SPRITE):
        with open(REFERENCE_SPRITE, "rb") as handle:
            encoded = base64.b64encode(handle.read()).decode()
        content.append({"type": "image_url",
                        "image_url": {"url": "data:image/png;base64," + encoded}})
        print(f"reference: {display_path(REFERENCE_SPRITE)}")

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
            "HTTP-Referer": "https://localhost/airogue-tile-art",
            "X-Title": "AIRogue tile art",
        },
    )
    print(f"requesting: {MODEL} (1 request for {len(prompt)} bytes of prompt)")
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
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    with open(destination, "wb") as handle:
        handle.write(base64.b64decode(encoded))
    with open(os.path.splitext(destination)[0] + ".prompt.txt", "w") as handle:
        handle.write(prompt + "\n")
    print(f"wrote sheet: {display_path(destination)} "
          f"({os.path.getsize(destination)} bytes)")
    return payload.get("usage") or {}


# ---------------------------------------------------------------------------
# CUT + PALETTE, from the sheet
# ---------------------------------------------------------------------------

def magick(*args):
    result = subprocess.run(["magick", *args], capture_output=True, text=True,
                            check=False)
    if result.returncode != 0:
        sys.exit(f"magick failed: {' '.join(args)}\n{result.stderr.strip()}")
    return result.stdout


def require_tools():
    missing = []
    if subprocess.run(["magick", "-version"], capture_output=True).returncode != 0:
        missing.append("imagemagick")
    try:
        import PIL  # noqa: F401
    except ImportError:
        missing.append("python3Packages.pillow")
    if missing:
        sys.exit(
            "missing tools: " + ", ".join(missing) + "\n"
            "run this through nix-shell, e.g.\n"
            "  nix-shell -p " + " ".join(missing) + " --run "
            "\"...python3 tools/gen_tile_art.py ...\"")


def palette_png():
    """The palette as the reference *image* ImageMagick's -remap takes.

    `tools/palette.py derive` already writes data/palette.png and it is what the
    existing slicers remap to, so it is used directly. When it is missing there is
    nothing honest to fall back to: `-colors N` quantises, which is a different
    operation from snapping to a known set, and the palette contract this pipeline
    is checked against ("no opaque pixel outside the palette") would then be
    measured against a palette the sprite was never locked to. So the absence is
    reported and the sprite is left unlocked, which `--tile-art` then refuses --
    rather than producing something that looks locked and is not.
    """
    derived = os.path.join(ROOT, "data", "palette.png")
    return derived if os.path.exists(derived) else None


def cut_object_cell(sheet, cell_index, cols, rows, destination, palette_png_path,
                    inset):
    """One cell of an *object* sheet -> a 32x32 sprite with a transparent background.

    Objects are not surfaces, and the tile pipeline is wrong for them in the one way that
    matters: it deliberately produces an opaque, edge-to-edge image, because a tile's whole
    job is to fill its cell. An object sits *on* the ground, so an opaque background means
    it blots out whatever it is placed on. A generated object sprite was measured at
    1024/1024 opaque pixels with a solid corner, and in the world the object covered the
    tiles around it.

    So this path is a cut-out rather than a surface:

      key      the flat magenta background becomes fully transparent
      trim     to what is left, so the sprite is the object and not the cell
      pad      back out to a centred square, so the object keeps its proportions
      resize   NEAREST to TILE
      lock     palette-remap the colour, then restore the alpha

    The last step is the subtle one. `-remap` operates on colour and knows nothing about
    transparency, so remapping a keyed image directly would fill the transparent area with
    the nearest palette colour -- undoing the whole point. The alpha is therefore extracted
    before the remap and composited back after it.
    """
    cell_w = int(float(magick(sheet, "-format", "%[fx:w/" + str(cols) + "]", "info:").strip()))
    cell_h = int(float(magick(sheet, "-format", "%[fx:h/" + str(rows) + "]", "info:").strip()))
    col = cell_index % cols
    row = cell_index // cols

    crop = os.path.join(WORK, f"cell_{cell_index}_crop.png")
    magick(sheet, "-crop", f"{cell_w}x{cell_h}+{cell_w * col}+{cell_h * row}",
           "+repage", crop)

    # The key colour is the cell's own corner, the same way the tile path learns its gutter
    # -- the model renders the requested magenta a little darker and less saturated, and a
    # hardcoded #FF00FF matches nothing. A corner that is not magenta at all means no usable
    # key, which is reported rather than silently producing an opaque sprite.
    key_colour = pixel_at(crop, 0, 0)
    keyed = os.path.join(WORK, f"cell_{cell_index}_keyed.png")
    if not looks_like_magenta(key_colour):
        print(f"warning: cell {cell_index}: the background is not magenta "
              f"({key_colour}), so it cannot be removed and the sprite will be opaque",
              file=sys.stderr)
        magick(crop, "+repage", keyed)
    else:
        # `-fuzz` at the same tolerance the tile path uses for the gutter: the fringe
        # between subject and background is anti-aliased, and a hard colour match leaves a
        # ring of background around the object.
        fuzz = str(round(GUTTER_TOLERANCE * 100)) + "%"
        magick(crop, "-alpha", "set", "-fuzz", fuzz,
               "-transparent", f"rgb({key_colour[0]},{key_colour[1]},{key_colour[2]})",
               "+repage", keyed)

    # A key that removed nearly everything is a *failure*, not a result: it happens when the
    # subject is close in colour to its own background, so the key cannot tell them apart.
    # Measured on a real sheet asked for a stone object, the background was 91.7% of the cell
    # and the object was the same grey -- a border-connected flood removed 100% of it. A
    # fully transparent sprite is worse than an uncut one, because it hides the object's
    # glyph fallback and looks like the object vanished. So the uncut cell is used instead and
    # the failure is named.
    keyed_opaque = opaque_pixel_count(keyed)
    cell_pixels = max(1, cell_w * cell_h)
    if keyed_opaque < cell_pixels * 0.02:
        print(f"warning: cell {cell_index}: removing the background left only "
              f"{keyed_opaque}/{cell_pixels} pixel(s), so the subject is too close in "
              f"colour to its background to cut out. Using the uncut cell -- regenerate "
              f"with the magenta background the prompt asks for", file=sys.stderr)
        magick(crop, "+repage", keyed)

    # Trim to the object, then pad back to a square. Without the pad a tall object would be
    # stretched to the cell's aspect by the resize; with it the object keeps its proportions
    # and carries transparent margin, which `DrawSpriteFitted` centres.
    trimmed = os.path.join(WORK, f"cell_{cell_index}_trim.png")
    magick(keyed, "-trim", "+repage", trimmed)

    width, height = image_size(trimmed)
    if width < 1 or height < 1:
        # Everything was background. A sprite of pure transparency is worse than useless:
        # it hides the object's glyph fallback. Report and fall back to the untrimmed cell.
        print(f"warning: cell {cell_index}: nothing was left after removing the "
              f"background, so the untrimmed cell is used", file=sys.stderr)
        magick(keyed, "+repage", trimmed)
        width, height = image_size(trimmed)

    side = max(width, height)
    # A margin so the object does not touch the sprite's own edge either: at 32px, a subject
    # flush to the border looks like a cropped photo next to the pack's art.
    side = max(1, round(side * 1.06))
    padded = os.path.join(WORK, f"cell_{cell_index}_padded.png")
    magick(trimmed,
           "-background", "none",
           "-gravity", "center",
           "-extent", f"{side}x{side}",
           "+repage", padded)

    squared = os.path.join(WORK, f"cell_{cell_index}_squared.png")
    magick(padded, "-filter", "point", "-resize", f"{TILE}x{TILE}!", "+repage",
           squared)

    # Palette-lock the colour and keep the alpha. See the docstring: the alpha is lifted
    # out first because `-remap` would otherwise paint the transparent area.
    if palette_png_path:
        alpha = os.path.join(WORK, f"cell_{cell_index}_alpha.png")
        magick(squared, "-alpha", "extract", alpha)
        rgb = os.path.join(WORK, f"cell_{cell_index}_rgb.png")
        magick(squared, "-alpha", "off", rgb)
        locked_rgb = os.path.join(WORK, f"cell_{cell_index}_lockedrgb.png")
        magick(rgb, "-remap", palette_png_path, locked_rgb)
        magick(locked_rgb, alpha, "-alpha", "off", "-compose", "CopyOpacity",
               "-composite", "+repage", destination)
    else:
        magick(squared, "+repage", destination)

    opaque = opaque_pixel_count(destination)
    print(f"cut cell {cell_index}: {width}x{height} object, {opaque}/{TILE * TILE} "
          f"opaque pixel(s)")
    return destination


def cut_cell(sheet, cell_index, cols, rows, destination, palette_png_path, inset):
    """One cell -> a 32x32 tile sprite.

    The order is the constraint from tile-art-wiring-design.md: **palette lock before
    the NEAREST downscale, never after.** Locking at full resolution means the
    nearest-sample that follows is choosing between palette colours rather than between
    arbitrary interpolated ones, which is what keeps the downscale from inventing
    colours the palette does not contain.

    How much to cut is *measured*, not assumed. Two bugs lived here:

      * the gutter was keyed on pure magenta (#FF00FF), but the model emitted
        srgb(205,50,141) -- about 50 levels off on every channel -- so `-trim` matched
        nothing and removed nothing. The gutter now identifies itself as the cell's own
        edge colour, whatever it is;
      * the inset was a fixed 8 source pixels, which is 0.25 of a final pixel at the
        1024 -> 32 reduction a one-cell sheet produces, so it removed nothing either.
        It is now scaled by the reduction *and* taken past the rim the model actually
        drew, so a frame cannot survive into the sprite.

    A frame is fatal for a tile in a way it never is for an object: it repeats on every
    cell and reads as a grid across the whole floor (design §7.2).
    """
    cell_w = int(float(magick(sheet, "-format", "%[fx:w/" + str(cols) + "]", "info:").strip()))
    cell_h = int(float(magick(sheet, "-format", "%[fx:h/" + str(rows) + "]", "info:").strip()))
    col = cell_index % cols
    row = cell_index // cols

    crop = os.path.join(WORK, f"cell_{cell_index}_crop.png")
    magick(sheet, "-crop", f"{cell_w}x{cell_h}+{cell_w * col}+{cell_h * row}",
           "+repage", crop)

    # The rim: how far the model's frame reaches, measured per edge. Recorded as the
    # "trim" artifact because it is the stage that used to be a `-trim` and no longer
    # needs to be -- the geometry is read rather than inferred from a colour match.
    rim_left, rim_top, rim_right, rim_bottom = frame_inset(crop, cell_w, cell_h)
    trimmed = os.path.join(WORK, f"cell_{cell_index}_trim.png")
    magick(crop, "-crop",
           f"{max(1, cell_w - rim_left - rim_right)}x"
           f"{max(1, cell_h - rim_top - rim_bottom)}"
           f"+{rim_left}+{rim_top}",
           "+repage", trimmed)

    # Plus a margin from the anti-aliased edge where the texture met the gutter, and
    # from any rim the walk above could not see because it is not gutter-coloured.
    # Expressed in final pixels and scaled by the reduction, so it means the same thing
    # at any cell size.
    width, height = image_size(trimmed)
    reduction = max(1.0, width / TILE)
    margin = max(inset, round(INSET_TILE_PIXELS * reduction))
    left = min(margin, max(0, width // 4))
    top = min(margin, max(0, height // 4))
    right = max(left + 1, width - margin)
    bottom = max(top + 1, height - margin)

    # The measured crop, before the fringe is removed.
    margin_path = os.path.join(WORK, f"cell_{cell_index}_margin.png")
    magick(trimmed, "-crop", f"{right - left}x{bottom - top}+{left}+{top}",
           "+repage", "-alpha", "set", margin_path)

    # Then the anti-aliased band between the gutter and the art, deleted and filled
    # from the neighbouring art so the tile stays opaque and its edge is the texture.
    washed = os.path.join(WORK, f"cell_{cell_index}_washed.png")
    wash_gutter(margin_path, crop, washed)

    locked = os.path.join(WORK, f"cell_{cell_index}_locked.png")
    if palette_png_path:
        magick(washed, "-remap", palette_png_path, locked)
    else:
        magick(washed, "+repage", locked)

    # NEAREST, and explicitly so: the intent is hard pixel edges, and the default
    # filter would blur every one of them (design §7.2).
    #
    # The ratio of source pixels to output pixels is measured and reported, and it is
    # *only* a diagnostic. An earlier design note proposed using it to cap how many cells
    # may share a sheet, on the theory that more cells means fewer source pixels each.
    # That theory does not survive contact with the real sheets:
    #
    #   1-cell request -> the model returned 1024x1024 -> 1024/32 = 32.0 source px each
    #   4-cell request -> the model returned  400x400 ->  200/32 =  6.2 source px each
    #
    # The sheet size is chosen by the model, not by us, and it does not scale with the
    # cell count -- so the 4-cell sprites, which are the ones that came out usable, had
    # five times *less* source detail than the 1-cell sprite that went wrong. What ruined
    # that sprite was the wash stage eating 73% of the tile, not its resolution.
    #
    # So this is printed rather than used to refuse a cut: it tells an operator why a
    # sprite looks soft, and it is the number to watch when the first real multi-cell set
    # is generated. Nothing gates on it, because the data does not support a threshold --
    # see docs/sprite-designer-design.md §16.
    #
    # Measured per axis as a plain ratio: 1024 -> 32 is "32 source px per output px",
    # meaning each side of the output was reduced from a 32-times-longer side. The first
    # version of this line squared that (1024 "source px per output px") and the second
    # multiplied the two axis ratios together (also 1024), which is an area and reads as
    # a far more alarming number for the same image. One side, one number.
    if width and height:
        source_ratio = width / float(TILE)
        print(f"cut cell {cell_index}: {width}x{height} -> {TILE}x{TILE} "
              f"({source_ratio:.1f} source px per output px)")

    magick(locked, "-filter", "point", "-resize", f"{TILE}x{TILE}!", "+repage",
           destination)
    return destination


def image_size(path):
    out = magick(path, "-format", "%w %h", "info:").split()
    return int(out[0]), int(out[1])


def pixel_at(path, x, y):
    """One pixel as an (r, g, b) triple."""
    out = magick(path, "-format", f"%[pixel:p{{{x},{y}}}]", "info:").strip()
    numbers = re.findall(r"\d+", out)
    if len(numbers) < 3:
        return (0, 0, 0)
    return tuple(int(n) for n in numbers[:3])


def opaque_pixel_count(path):
    """How many pixels of `path` are more than half opaque.

    One ImageMagick call rather than a pixel loop: the alpha channel's mean times the pixel
    count is the number of opaque pixels, which is the figure worth reporting for a cut-out
    (and the one that distinguishes "the object survived the key" from "the key ate it").
    """
    out = magick(path, "-alpha", "extract", "-format", "%[fx:mean*w*h]", "info:").strip()
    try:
        return int(round(float(out)))
    except (TypeError, ValueError):
        return 0


def looks_like_gutter(pixel, gutter):
    """Whether `pixel` is within tolerance of the gutter colour.

    Compared per channel against the full range rather than as a Euclidean distance,
    because the failure this exists to catch is a uniform offset on every channel -- the
    model renders the requested magenta a little darker and less saturated -- and a
    per-channel bound expresses "the same colour, differently rendered" directly.
    """
    return all(abs(c - t) <= GUTTER_TOLERANCE * 255 for c, t in zip(pixel, gutter))


def looks_like_magenta(pixel):
    """Whether `pixel` could be the #FF00FF gutter the prompt asks for.

    This exists because assuming the cell's edge colour *is* the gutter is only valid
    when a gutter exists. For a single-cell sheet the model paints the surface edge to
    edge -- which is what the cell description asks for -- so the "gutter" sampled from
    the corner is a real texture colour. On a dark brick floor (edge `srgb(66,46,38)`)
    every brick and mortar pixel then falls within `GUTTER_TOLERANCE` of it, and both
    this stage and `frame_inset` consume the art: measured on a real one-cell
    generation, `wash_gutter` replaced **580,380 of ~795,000 pixels (73%)**, turning a
    clean brick floor into a flat reddish-brown blob. "No gutter is present" is
    therefore a state that has to be detected rather than assumed away.

    Magenta is defined by its *relationship* -- red and blue both far above a suppressed
    green, and far enough from grey that a stone or a skin tone cannot pass -- rather
    than by absolute channel floors. That distinction is not pedantry: the floors this
    replaced (`r >= 140 and b >= 140 and g <= 120`) were tuned to one earlier sample,
    `srgb(205,50,141)`, whose blue was *one unit* over the floor. The very next observed
    generation came back `srgb(215,52,135)` -- blue 135, five under that floor -- so the
    key was skipped, the uncut cell was used, the palette lock mapped the near-magenta
    to a red, and the object arrived in the world with an opaque red background. A guard
    that passes by one unit is not a guard. What the two samples have in common is the
    relationship, not the channels.

    Checked against every magenta the model has been observed to emit (`(255,0,255)`,
    `(205,50,141)`, `(215,52,135)`) and against the texture colours that must not match
    (brick, mortar, stone, cream wall, skin tone, and the palette red an un-keyed
    background remaps to). `--cut-check` asserts exactly that.
    """
    r, g, b = pixel[0], pixel[1], pixel[2]
    # Red and blue both far above green, and far from grey: a texture's red and blue
    # track its green, so `g * 2 < min(r, b)` is the part that rejects the brick and the
    # skin tone while accepting a magenta the model rendered darker and less saturated.
    return r > 110 and b > 100 and g * 2 < min(r, b)


def wash_gutter(path, source_crop, destination):
    """Removes the gutter and its anti-aliased fringe from the edge of a cropped cell.

    Both problems this solves were measured, not guessed:

      * `frame_inset` stops at the first pixel that is not gutter-coloured, which is the
        *start* of the fringe between gutter and art. Those pixels are opaque, survive
        into the sprite, and the palette lock snaps the reddish ones to a red -- the mesh
        border, 70 pixels of 1024 on a freshly generated tile;
      * a border-connected flood cannot reach all of them: the art's own outline
        interrupts connectivity, so 22,070 of the 91,280 border-band pixels were still
        magenta after the first attempt at this.

    So the fringe is taken as *every* pixel within tolerance of the cell's edge colour,
    not only the border-connected ones -- a magenta pixel anywhere near the edge of a
    tile is gutter, there is nothing else it can be -- and each is filled from the
    nearest pixel that is not fringe, by a breadth-first sweep outwards. A multi-source
    BFS rather than a per-pixel expanding search, because that is what makes the fill
    complete: the band can be tens of pixels wide at source resolution.

    `source_crop` must be the **uncropped cell**, because the gutter colour is read from
    its corner: the margin crop has already moved that corner inwards, so a corner of it
    can be art rather than background.
    """
    gutter = pixel_at(source_crop, 0, 0)
    if not looks_like_magenta(gutter):
        # No gutter was drawn, so there is no fringe and no gutter colour to match
        # against. Writing the cell through unchanged is the whole fix: everything
        # below would otherwise treat a real texture colour as the gutter and flood
        # the tile.
        magick(path, "+repage", "-alpha", "set", destination)
        return 0
    limit = GUTTER_TOLERANCE * 255

    image = Image.open(path).convert("RGBA")
    width, height = image.size
    pixels = image.load()

    def is_gutter(x, y):
        r, g, b, a = pixels[x, y]
        if a == 0:
            return True
        return all(abs(c - t) <= limit for c, t in zip((r, g, b), gutter))

    fringe = {(x, y)
              for y in range(height)
              for x in range(width)
              if is_gutter(x, y)}

    if not fringe:
        image.save(destination)
        return 0

    # Multi-source BFS: art pixels are the sources, fringe pixels take the colour of
    # whichever source is fewest steps away. Every fringe pixel is therefore filled,
    # and a fringe pixel with no art anywhere becomes transparent rather than faking a
    # colour -- which the remap then clears.
    from collections import deque

    distance = {}
    queue = deque()
    for x, y in fringe:
        pixels[x, y] = (0, 0, 0, 0)
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < width and 0 <= ny < height and (nx, ny) not in fringe:
                distance[(nx, ny)] = 0
                queue.append((nx, ny))

    seen = set(distance)
    while queue:
        x, y = queue.popleft()
        source = pixels[x, y]
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if not (0 <= nx < width and 0 <= ny < height):
                continue
            if (nx, ny) in seen:
                continue
            seen.add((nx, ny))
            if (nx, ny) in fringe:
                pixels[nx, ny] = source
            queue.append((nx, ny))

    image.save(destination)
    return len(fringe)


def frame_inset(path, width, height):
    """How many pixels of gutter or frame surround the art, per edge.

    The cell's own edge colour identifies the gutter, and each edge is then walked
    inwards to the first pixel that is *not* gutter. That is what makes this work where
    a fixed inset and a `-trim` against #FF00FF both failed: the tolerance is measured
    against the colour the model actually drew, and the distance is measured rather
    than guessed.

    Only meaningful when a gutter was actually drawn -- see `looks_like_magenta`. With
    no gutter the edge colour is a texture colour, and walking to the first pixel that
    is not within tolerance of it would trim real brickwork off a one-cell sheet.
    """
    gutter = pixel_at(path, 0, 0)
    if not looks_like_magenta(gutter):
        return 0, 0, 0, 0
    margin = max(1, round(min(width, height) * MAX_FRAME_FRACTION))

    def walk(pixel_of, limit):
        depth = 0
        while depth < limit:
            if not looks_like_gutter(pixel_of(depth), gutter):
                break
            depth += 1
        return depth

    left = walk(lambda d: pixel_at(path, d, height // 2), min(margin, width // 2))
    right = walk(lambda d: pixel_at(path, width - 1 - d, height // 2),
                 min(margin, width // 2))
    top = walk(lambda d: pixel_at(path, width // 2, d), min(margin, height // 2))
    bottom = walk(lambda d: pixel_at(path, width // 2, height - 1 - d),
                  min(margin, height // 2))
    return left, top, right, bottom


# ---------------------------------------------------------------------------

def parse_args():
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("names", nargs="*", metavar="TILE",
                        help="tile names, as the catalog spells them")
    parser.add_argument("--from-cells", default="", metavar="FILE",
                        help="draw the cells a normalized worklist names, honouring the "
                             "kind and convention id per cell. This is how "
                             "--from-worklist hands work to the child process")
    parser.add_argument("--from-worklist", default="", metavar="FILE",
                        help="read the tiles to draw from a worklist written by "
                             "the game (TileArtQueue) and write "
                             "<FILE>.result.json. This is how the in-game queue "
                             "drives generation")
    parser.add_argument("--dry-run", action="store_true",
                        help="build and print the prompt, write nothing, spend "
                             "nothing")
    parser.add_argument("--cut-check", action="store_true",
                        help="run the cut stage's own self-test on synthetic sheets "
                             "(the colour logic, no files, no cost) and exit")
    parser.add_argument("--sheet", default="",
                        help="slice an existing sheet instead of generating one. "
                             "Free -- use it to re-cut or re-lock without paying "
                             "again")
    parser.add_argument("--cols", type=int, default=0,
                        help="cells per row in the sheet (default: one row)")
    parser.add_argument("--inset", type=int, default=INSET_TILE_PIXELS,
                        help="minimum pixels removed from each edge of the source cell, "
                             "before the reduction (default "
                             f"{INSET_TILE_PIXELS} * reduction; the measured rim is "
                             "always removed on top of this)")
    parser.add_argument("--out", default="",
                        help="output directory for the 32px sprites "
                             "(default: the generated layer's sprites/)")
    parser.add_argument("--settings", default="",
                        help="settings.json to read the generated layer from")
    args = parser.parse_args()
    if (not args.names and not args.from_worklist and not args.from_cells
            and not args.cut_check):
        parser.error("give at least one TILE, or --from-worklist FILE")
    # `--from-cells` is how `--from-worklist` re-invokes this script, so a worklist run has
    # both; the two name lists themselves are mutually exclusive.
    if args.names and args.from_worklist:
        parser.error("give TILEs or --from-worklist, not both")
    if args.names and args.from_cells:
        parser.error("give TILEs or --from-cells, not both")
    return args


def write_result(worklist_path, ok, tiles, error="", cost=0.0):
    """The game's contract: <worklist>.result.json says what happened.

    Written on every exit path, including failure, because the game's worker reads
    this file and nothing else -- a missing file is indistinguishable from a tool
    that never ran. `error` is the sentence the tile designer's window shows.
    """
    payload = {"ok": ok, "tiles": tiles, "error": error, "cost": cost}
    with open(worklist_path + ".result.json", "w") as handle:
        json.dump(payload, handle, indent=2)
        handle.write("\n")


def grid_columns(count):
    """How many cells per row to choose for `count` tiles in one sheet.

    Not `ceil(sqrt(count))`, which is the obvious near-square and is what this used to
    do. A square grid wastes cells whenever the count is not a perfect square: eight
    sprites become 3x3 with one cell empty, which is a ninth of the sheet's area spent on
    nothing after the request has already been paid for.

    Instead the widest shape is picked whose row count still fills every cell -- the
    smallest divisor of `count` that is at least `sqrt(count)`. Eight becomes 4x2, not
    3x3; twelve becomes 4x3; a prime count stays 1xN or Nx1, which is the shape that
    cannot waste anything. Near-square is preserved because a very lopsided grid is harder
    for the model to count cells in, and an off-by-one row is exactly the failure that
    produces a repeated motif.

    A single cell stays 1x1 and a count of 1 must not fall through to a 1x1x1 grid with
    the "*1" semantics of a divisor loop, hence the early return.
    """
    if count <= 1:
        return 1
    target = math.isqrt(count)
    if target * target == count:
        return target
    # The largest divisor not exceeding sqrt is the *tallest* legal grid; the one at or
    # above sqrt is the widest. Widest is chosen so a set of eight reads 4x2 rather than
    # 2x4, matching the reading order the prompt describes.
    for divisor in range(target, 0, -1):
        if count % divisor == 0:
            return count // divisor
    return count


def worklist_cells(worklist):
    """The worklist's cells, normalized to {"name", "kind", "conventionId"}.

    Accepts both spellings the engine has written: the original flat
    `"tiles": ["a", "b"]`, and the `"cells": [{...}]` form that carries the kind. An old
    worklist -- or a hand-written one -- therefore still reads, and its names are treated
    as tiles, which is what they were.

    The convention id is filled in here when the writer did not supply it, using the same
    per-kind rule the engine uses. That keeps one authority for the filename: the engine,
    not this script (see the comment in `slugify`).
    """
    cells = []
    for raw in worklist.get("cells") or []:
        if isinstance(raw, str):
            raw = {"name": raw}
        if not isinstance(raw, dict):
            continue
        name = str(raw.get("name") or "")
        if not name:
            continue
        kind = str(raw.get("kind") or "tile")
        convention = str(raw.get("conventionId") or "")
        if not convention:
            convention = slugify(name) if kind == "tile" else slugify(name)[len("tile_"):]
        cells.append({"name": name, "kind": kind, "conventionId": convention})
    if cells:
        return cells
    for raw in worklist.get("tiles") or []:
        name = str(raw)
        if not name:
            continue
        cells.append({"name": name, "kind": "tile", "conventionId": slugify(name)})
    return cells


def run_from_worklist(worklist_path, dry_run=False):
    """Draw the tiles a worklist names, then report. Never raises: the game is
    waiting on a result file, so every failure has to come back as one.

    `dry_run` builds and logs the prompt without sending a request, and answers with
    a result that says so. That makes the whole game -> worklist -> tool path
    verifiable for free, which is the only way to check the invocation itself (the
    nix-shell wrapper, the paths, the result file) before agreeing to spend.
    """
    try:
        with open(worklist_path) as handle:
            worklist = json.load(handle)
    except (OSError, ValueError) as error:
        write_result(worklist_path, False, [], f"unreadable worklist: {error}")
        return 1

    cells = worklist_cells(worklist)
    if not cells:
        write_result(worklist_path, False, [], "the worklist names no tiles")
        return 1

    # The normalized cells are written beside the worklist and handed to the child, because
    # the kind and the convention id have to survive the re-invocation. Passing the names
    # as positional arguments -- which is what this used to do -- lost both, and the loss
    # was silent: an object would come back named as a tile and the run would look fine.
    cells_path = worklist_path + ".cells.json"
    with open(cells_path, "w") as handle:
        json.dump({"cells": cells}, handle, indent=2)
        handle.write("\n")

    # The log goes beside the worklist, because the game runs this detached from a
    # terminal: without it a failure is a single sentence in the window and nothing
    # else to look at.
    log_path = worklist_path + ".log"
    argv = [sys.executable, os.path.abspath(__file__)]
    argv += ["--cols", str(grid_columns(len(cells)))]
    if dry_run:
        argv += ["--dry-run"]
    if worklist.get("spritesDirectory"):
        argv += ["--out", str(worklist["spritesDirectory"])]
    if worklist.get("paletteJson"):
        # Not used as an argument (the palette is found by convention) but recorded
        # so the log shows which settings the run came from.
        argv += ["--settings", os.path.join(ROOT, "settings.json")]
    argv += ["--from-cells", cells_path]
    tiles = [cell["name"] for cell in cells]

    with open(log_path, "w") as log:
        log.write("worklist: " + worklist_path + "\n")
        log.write("argv: " + " ".join(argv) + "\n\n")
        log.flush()
        result = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT,
                                text=True, check=False)

    # The generator reports its cost on stdout, which is in the log. Read it back
    # rather than not reporting a number the user is paying for.
    cost = 0.0
    try:
        with open(log_path) as log:
            for line in log:
                if line.startswith("cost:"):
                    parts = line.split()
                    if len(parts) > 1:
                        cost = float(parts[1])
    except (OSError, ValueError):
        pass

    if dry_run:
        # Nothing was drawn, so `ok` is false on purpose: the queue reads this as the
        # job failing, which is correct -- no sprite was produced -- and the error
        # sentence says the run was a dry one rather than a broken tool.
        write_result(worklist_path, False, [], "dry run: the prompt was built and "
                                               "logged, and no request was sent")
        return 0

    if result.returncode != 0:
        write_result(worklist_path,
                     False, [],
                     f"generation failed (exit {result.returncode}); see {log_path}",
                     cost)
        return result.returncode

    write_result(worklist_path, True, tiles, "", cost)
    return 0


def generated_sprites_dir(settings_path):
    """The generated layer's sprites directory, from settings, or the default.

    Derived the same way SpritePaths::DerivedGeneratedDirectory does --
    <pack's grandparent>/generated/<pack>/sprites -- so this writes where the
    engine looks. With no art pack configured there is no generated layer at all
    (data/sprites/ *is* the art), and writing there would put generated output
    where hand-authored art lives, so it is refused rather than guessed at.
    """
    path = settings_path or os.path.join(ROOT, "settings.json")
    settings = {}
    if os.path.exists(path):
        with open(path) as handle:
            settings = json.load(handle)
    art = settings.get("art") or {}
    pack = art.get("pack") or ""
    if not pack:
        return ""
    if not os.path.isabs(pack):
        pack = os.path.join(ROOT, pack)
    if art.get("generated"):
        generated = art["generated"]
        if not os.path.isabs(generated):
            generated = os.path.join(ROOT, generated)
        return generated
    grandparent = os.path.dirname(os.path.dirname(pack))
    return os.path.join(grandparent, "generated", os.path.basename(pack), "sprites")


# ---------------------------------------------------------------------------
# The cut stage's self-test
# ---------------------------------------------------------------------------

def cut_check():
    """Proves the cut stage does not eat a tile that has no gutter.

    The bug this exists for: `frame_inset` and `wash_gutter` both identified the gutter
    as "the cell's own edge colour, within `GUTTER_TOLERANCE`". On a single-cell sheet
    the model paints the surface edge to edge -- which is what the cell description asks
    for -- so the edge colour is a real texture colour. On a dark brick floor
    (`srgb(66,46,38)`) every brick and mortar pixel is within 56 per channel of it, and
    the wash replaced **580,380 of ~795,000 pixels (73%)**: a clean brick floor came out
    as a flat reddish-brown blob. Every existing test passed, because every sheet tested
    until then had a gutter.

    Synthetic and pure: no files are read, no request is made, no sprites are written.
    It exercises the colour logic that decides how much of a cell survives, which is
    where the damage was.
    """
    failures = []

    def check(name, got, want):
        if got != want:
            failures.append(f"{name}: got {got!r}, wanted {want!r}")

    # The magenta key. Three values the model has actually emitted for the #FF00FF the
    # prompt asks for: the nominal one, the first sample the old thresholds were tuned
    # to, and the one that revealed the thresholds were wrong.
    #
    # The third is the regression test. `srgb(215,52,135)` is what the real object sheet
    # came back with, and it is what the replaced absolute floors
    # (`r >= 140 and b >= 140 and g <= 120`) rejected -- by five on blue -- leaving the
    # sprite opaque and palette-mapped to a red. A `looks_like_magenta` that has crept
    # back to a channel floor fails here, which is the whole point of keeping it.
    for name, pixel in (("pure magenta #FF00FF", (255, 0, 255)),
                        ("the model's srgb(205,50,141)", (205, 50, 141)),
                        ("the real sheet's srgb(215,52,135)", (215, 52, 135))):
        check(f"{name} is a gutter", looks_like_magenta(pixel), True)
    # Named separately, because it is the property and not the sample that matters: a
    # magenta whose blue is below any plausible absolute floor is still a magenta. The
    # predicate must not be able to pass this by accident.
    check("a magenta with blue below 140 is still a gutter",
          looks_like_magenta((215, 52, 135)), True)

    # Texture colours must never be mistaken for one. The brick values are sampled from
    # the real clay brick floor sheet that the first version of this test guarded, and the
    # palette red is the colour an un-keyed magenta background remaps to -- the exact
    # colour the owner saw as an opaque background.
    for name, pixel in (("dark brick mortar", (66, 46, 38)),
                        ("the dark brick", (87, 50, 39)),
                        ("the mid brick", (112, 61, 40)),
                        ("light brick", (178, 110, 80)),
                        ("red brick highlight", (226, 155, 54)),
                        ("another red brick highlight", (198, 110, 80)),
                        ("grey stone", (120, 120, 118)),
                        ("cream wall", (237, 226, 203)),
                        ("a skin tone", (210, 158, 120)),
                        ("the palette red an un-keyed background becomes", (180, 47, 47))):
        check(f"{name} is not a gutter", looks_like_magenta(pixel), False)

    # Real brickwork *is* within tolerance of the mortar: on the sheet the bug destroyed,
    # 284,672 of 1,048,576 pixels (~27%) were, including four of the sprite's own
    # colours. That is why proximity alone can never separate gutter from texture, and
    # why the magenta test is the thing doing the work.
    mortar = (66, 46, 38)
    for name, pixel in (("the dark brick", (87, 50, 39)),
                        ("the mid brick", (112, 61, 40)),
                        ("a second mortar speckle", (90, 70, 63))):
        check(f"{name} is within tolerance of the mortar",
              looks_like_gutter(pixel, mortar), True)
    # ...while a light brick is not, so the test above is not vacuously true.
    check("a light brick is outside tolerance",
          looks_like_gutter((178, 110, 80), mortar), False)

    # End to end on a two-colour sheet: with a magenta gutter it is removed, and with a
    # brick edge it is left alone. Both go through the same functions the pipeline uses.
    import tempfile
    from PIL import Image as _Image
    with tempfile.TemporaryDirectory() as directory:
        def write_png(path, corner, art, size=64):
            image = _Image.new("RGBA", (size, size), corner + (255,))
            for y in range(size // 4, size * 3 // 4):
                for x in range(size // 4, size * 3 // 4):
                    image.putpixel((x, y), art + (255,))
            image.save(path)

        # A gutter case: magenta edge, brick art in the middle.
        guttered = os.path.join(directory, "gutter.png")
        write_png(guttered, (255, 0, 255), (178, 110, 80))
        washed = os.path.join(directory, "gutter_washed.png")
        removed = wash_gutter(guttered, guttered, washed)
        check("a magenta gutter is removed", removed > 0, True)
        in_gutter = pixel_at(washed, 0, 0)
        check("the gutter pixel took the art's colour", in_gutter, (178, 110, 80))
        check("the gutter case trims", frame_inset(guttered, 64, 64) != (0, 0, 0, 0), True)

        # The reported case: a brick edge and brick art, no gutter anywhere.
        brick = os.path.join(directory, "brick.png")
        write_png(brick, (66, 46, 38), (178, 110, 80))
        washed_brick = os.path.join(directory, "brick_washed.png")
        replaced = wash_gutter(brick, brick, washed_brick)
        check("a tile with no gutter survives untouched", replaced, 0)
        check("the brick edge is not trimmed", frame_inset(brick, 64, 64), (0, 0, 0, 0))
        check("the corner is still the brick colour", pixel_at(washed_brick, 0, 0), (66, 46, 38))
        check("the art is still the art", pixel_at(washed_brick, 32, 32), (178, 110, 80))

    if failures:
        for failure in failures:
            print("cut check FAILED: " + failure)
        return 1
    print("cut check:  ok (a gutter is removed; a full-bleed tile with no gutter "
          "survives intact)")
    return 0


def main():
    args = parse_args()
    if args.cut_check:
        return cut_check()

    # The queue's path: the game wrote a worklist, this draws it, and the result
    # goes to <worklist>.result.json. Checked first because it replaces the whole
    # normal flow rather than adding to it.
    if args.from_worklist:
        # `--dry-run` is honoured here too, so the game's own invocation can be
        # proven without a request: same command, same paths, same result file.
        return run_from_worklist(args.from_worklist, dry_run=args.dry_run)

    definitions = load_tile_definitions()
    palette = load_palette()

    # An unknown name is not fatal, and deliberately still drawn. This used to crash:
    # `build_prompt` indexes `definitions[name]`, so an unknown name was a KeyError inside
    # a subprocess, which the game sees as "no result file" with no explanation. A
    # placeholder is used instead -- the walk below fills one for any cell the catalog does
    # not have, so a caller that asked for a name gets a sprite with that name.
    def placeholder(name):
        return {
            "name": name,
            "characterColor": [0xF5, 0xDC, 0x56],
            "backgroundColor": [0x48, 0x48, 0x45],
        }

    # The cells to draw. Two sources, and the difference is whether a kind is known:
    #
    #   --from-cells  the engine wrote it, so every cell names its kind and convention id
    #   bare names    a hand-run CLI invocation, which is a tile request
    #
    # A bare name list is a tile because that is what every old invocation meant, and
    # because guessing from the name is exactly the heuristic that made an object called
    # "stone wall" come back as a texture. Recorded, not inferred.
    if args.from_cells:
        try:
            with open(args.from_cells) as handle:
                cells = worklist_cells(json.load(handle))
        except (OSError, ValueError) as error:
            sys.exit(f"could not read --from-cells {args.from_cells}: {error}")
        if not cells:
            sys.exit(f"--from-cells {args.from_cells} names no cells")
    else:
        cells = [{"name": name, "kind": "tile", "conventionId": slugify(name)}
                 for name in args.names]
    names = [cell["name"] for cell in cells]
    kinds = {cell["name"]: cell["kind"] for cell in cells}

    # A definition the catalog does not have is still drawn. The placeholder is keyed by
    # name, so the kinds above are unaffected.
    unknown = [cell["name"] for cell in cells if cell["name"] not in definitions]
    if unknown:
        for name in unknown:
            definitions[name] = placeholder(name)
        print(f"note: no definition found for: {', '.join(unknown)} "
              f"(drawing from the name alone)", file=sys.stderr)

    cols = args.cols if args.cols > 0 else len(cells)
    rows = math.ceil(len(cells) / cols)
    prompt = build_prompt(cells, definitions, palette, cols)

    out_dir = args.out or generated_sprites_dir(args.settings)
    if not out_dir:
        sys.exit(
            "no generated layer is configured, so there is nowhere to write that "
            "would not be the hand-authored art.\n"
            "Set art.pack in settings.json (the generated layer is derived from "
            "it), or pass --out explicitly.")

    print(f"cells:      {', '.join(names)}")
    print(f"kinds:      " + ", ".join(
        f"{cell['name']}={cell['kind'] or 'tile'}" for cell in cells))
    print(f"grid:       {cols} x {rows}  ({len(cells)} cell(s), "
          f"{cols * rows - len(cells)} empty)")
    print(f"palette:    {len(palette)} colour(s) from {display_path(PALETTE_JSON)}"
          if palette else "palette:    (none derived -- colours will not be locked)")
    print(f"output:     {display_path(out_dir)}/")
    # The convention id, printed per cell: it is the filename, and it is the one thing a
    # wrong kind would silently change.
    for cell in cells:
        print(f"            {cell['conventionId']}.png   <- \"{cell['name']}\"")
    print()

    # The prompt is only worth printing when it is going to be sent (or when the
    # point of the run *is* to read it). A --sheet re-cut spends nothing and does
    # not use it, so printing a thousand characters of it would bury the result.
    if args.dry_run or not args.sheet:
        print(prompt)
        print()

    if args.dry_run:
        print("--dry-run: nothing requested, nothing written, nothing spent.")
        return 0

    require_tools()

    if args.sheet:
        sheet = args.sheet if os.path.isabs(args.sheet) else os.path.join(ROOT, args.sheet)
        if not os.path.exists(sheet):
            sys.exit(f"--sheet does not exist: {sheet}")
        print(f"re-using sheet: {display_path(sheet)}  (no request, no cost)")
        usage = {}
    else:
        os.makedirs(WORK, exist_ok=True)
        sheet = os.path.join(WORK, "tile_art_sheet.png")
        usage = generate_sheet(prompt, sheet)

    os.makedirs(out_dir, exist_ok=True)
    palette_png_path = palette_png()
    if not palette_png_path:
        print("warning: no palette image to lock to; sprites will NOT be "
              "palette-locked and --tile-art will refuse them", file=sys.stderr)

    for index, name in enumerate(names):
        cell = cells[index]
        destination = os.path.join(out_dir, cell["conventionId"] + ".png")
        # The cut is where the two kinds genuinely diverge, and it is not cosmetic: a tile
        # must fill its cell and an object must not, so the same pipeline cannot serve both.
        # See cut_object_cell's docstring for what goes wrong when it tries.
        if cell["kind"] == "object":
            cut_object_cell(sheet, index, cols, rows, destination, palette_png_path,
                            args.inset)
        else:
            cut_cell(sheet, index, cols, rows, destination, palette_png_path, args.inset)
        print(f"wrote {display_path(destination)}")

    # The tile sheet is kept, always (§11.2): it is the only thing here that cost
    # money, and re-cutting from it is free.
    print(f"\nsheet kept at {display_path(sheet)} -- re-cut it with --sheet, free")
    if usage:
        image_tokens = (usage.get("completion_tokens_details") or {}).get("image_tokens")
        print(f"cost: {usage.get('cost')}  prompt_tokens: {usage.get('prompt_tokens')}  "
              f"image_tokens: {image_tokens}")
    return 0


if __name__ == "__main__":
    # Last-resort guard. When the game drives this, the only channel back is
    # `<worklist>.result.json`, so an uncaught exception is not a traceback anyone
    # sees -- it is a *missing* result file, which the queue reports as "the command
    # produced no result file" with no clue what went wrong. Catching here and
    # writing a result turns every unexpected failure into a sentence the tile
    # designer's window displays.
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except BaseException as error:  # noqa: BLE001 - the point is to catch everything
        import traceback
        traceback.print_exc()
        worklist = ""
        for index, argument in enumerate(sys.argv):
            if argument == "--from-worklist" and index + 1 < len(sys.argv):
                worklist = sys.argv[index + 1]
                break
        if worklist:
            try:
                write_result(worklist, False, [],
                             f"the generator crashed: {type(error).__name__}: {error}")
            except OSError:
                pass
        sys.exit(1)
