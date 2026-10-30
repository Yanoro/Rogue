#!/usr/bin/env python3
"""Generate ALL map tile sprites in a single image, as a grid.

Scratch tooling; not part of the game build.

Tiles are not objects, and the prompt says so in four ways that each fix a
different visible failure mode:

 * SEAMLESS. A tile repeats across the map, so its left edge meets its own right
   edge in the next tile over. A texture with a frame, a border or a vignette
   becomes a visible grid across the whole floor rather than one bad sprite.
 * FLAT AND TOP-DOWN. These are ground surfaces seen from directly above, so
   perspective, a horizon or a lighting gradient would make a floor look like a
   stack of pictures.
 * NO PROPS. A floor must not contain an object: a tree or a barrel baked into a
   floor tile would be duplicated on every one of them.
 * A CLEAR GUTTER BETWEEN CELLS. This one is for the pipeline, not the look.
   Unlike objects, tile textures are full-bleed, so adjacent cells would fuse
   into one blob and connected-component slicing could not separate them. The
   gutter is requested explicitly so the cells can be cut apart; it is cropped
   off afterwards, and "no border or outline around a texture" is asked for
   separately so the gutter does not get baked in as a frame.

Usage:  python3 sprite_preview/gen_tile_sheet.py
"""

import base64
import json
import os
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

MODEL = "google/gemini-2.5-flash-image"
API = "https://openrouter.ai/api/v1/chat/completions"
OUT = os.path.join(HERE, "raw", "tiles_sheet.png")
# The object sheet, used as a palette/style anchor: it already shows this game's
# stone, earth and wood in the target rendering style.
REFERENCE = os.path.join(HERE, "raw", "objects_sheet.png")

# Reading order, left to right, top to bottom, four per row. Colours are taken
# from each tile's backgroundColor/characterColor in the map files, so the
# texture keeps the tone the ASCII tile already had.
TILES = [
    "floor: a light grey stone flagstone floor, irregular flat flagstones with "
    "subtle darker grout lines between them",
    "wooden wall: a dark brown wooden wall of vertical timber planks with "
    "visible wood grain and darker shadowed gaps between the planks",
    "grass: a green grass field of short grass blades in dark and mid green",
    "fence: a weathered wooden fence of horizontal brown timber rails with "
    "vertical posts, on dark earth",
    "tilled soil: dark brown farm soil with parallel plough furrows",
    "path: a pale tan packed-earth path with small pebbles and gravel",
    "water: deep blue water with lighter blue ripple highlights",
    "stone wall: regular rectangular grey-blue stone blocks with light mortar "
    "lines between them",
    "packed earth: mid brown compacted dirt with fine speckles",
    "rock face: dark grey angular rock chunks and cracks",
    "wooden floor: a warm mid brown interior plank floor of long horizontal "
    "boards with visible wood grain and thin dark lines between the planks",
]

STYLE = (
    "A 16-bit pixel art tileset: a grid of 4 columns by 4 rows of equal square "
    "cells. Fill only the first eleven cells, in reading order, left to right then "
    "top to bottom, with these ground textures in this exact order: "
    + "; ".join(f"{index + 1}) {tile}" for index, tile in enumerate(TILES))
    + ". Leave the remaining five cells completely empty, showing only the flat "
    "background colour. "
    "Every texture is a TOP-DOWN view of the ground, seen from directly above, "
    "flat and evenly lit: no perspective, no horizon, no cast shadows and no "
    "lighting gradient. Fill each cell edge to edge with its texture, confined "
    "strictly to its own cell. "
    "CRITICAL TILING: every texture must be seamless and repeatable -- the left "
    "edge of a texture must continue perfectly into its right edge and its top "
    "edge into its bottom edge, so that placing many copies side by side produces "
    "one continuous surface with no visible seam. Each texture must also be "
    "uniform in tone across the cell, with no darker or lighter edges, no "
    "vignette, and no border, outline, frame or drop shadow around it. "
    "Do not draw any object, creature, plant, tool, building or character in any "
    "cell; these are ground surfaces only, and a prop baked into a tile would be "
    "repeated on every single one of them. No text, labels, numbers or grid lines. "
    "STYLE: chunky retro pixel art with a limited palette of at most 12 colours, "
    "matching the look of the attached reference image -- the same outline-free "
    "flat shading, the same kind of limited palette and the same level of detail. "
    "The attached image is a style reference only: do not draw any of the objects "
    "in it. "
    "Leave a clear gap of flat background colour between neighbouring cells, and "
    "make that gap even and obvious, so the cells are cleanly separated; the gap "
    "must be background colour, not a drawn border around the texture. The "
    "background must be one flat, uniform, fully saturated magenta colour "
    "(#FF00FF)."
)


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


def main():
    content = [{"type": "text", "text": STYLE}]
    if os.path.exists(REFERENCE):
        with open(REFERENCE, "rb") as handle:
            encoded = base64.b64encode(handle.read()).decode()
        content.append(
            {"type": "image_url",
             "image_url": {"url": "data:image/png;base64," + encoded}}
        )
        print(f"attaching style reference: {REFERENCE}")
    else:
        print("no style reference attached")

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
            "X-Title": "AIRogue tile sheet",
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
    with open(os.path.join(HERE, "raw", "tiles_sheet.prompt.txt"), "w") as handle:
        handle.write(STYLE + "\n")

    usage = payload.get("usage") or {}
    print(f"wrote {OUT} ({os.path.getsize(OUT)} bytes)")
    print(f"cost: {usage.get('cost')}  prompt_tokens: {usage.get('prompt_tokens')}  "
          f"image_tokens: {(usage.get('completion_tokens_details') or {}).get('image_tokens')}")


if __name__ == "__main__":
    main()
