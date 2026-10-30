#!/usr/bin/env python3
"""Generate all four profession sprites in ONE image, for style consistency.

Scratch tooling for evaluating AI-generated sprites; not part of the game build.

Why one request instead of four: image models drift in style between separate
generations, and a set of agents has to look like one set. One lineup also costs
one request instead of four. The tradeoff is that the result must be sliced, which
slice_sprites.py does by connected-component analysis rather than by guessing at
a grid.

The prompt bans anti-aliasing explicitly. That is not cosmetic: soft silhouette
edges are what produced the magenta fringe in the first test, because a magenta
background blends into the character over 1-2 px that no chroma key can cleanly
separate at 32px. A hard-edged source keys to a clean matte.

Usage:  python3 sprite_preview/gen_lineup.py
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
OUT = os.path.join(HERE, "raw", "lineup.png")

CAST = [
    "farmer with a wide-brimmed straw hat, rolled-up beige tunic and a wooden pitchfork held upright",
    "miller with a flour-dusted white apron, a soft dust cap and a tied flour sack on one shoulder",
    "blacksmith with a heavy leather apron, bare muscular arms and a blacksmith's hammer",
    "travelling trader with a wide felt hat, a colourful layered cloak and a leather satchel",
]

PROMPT = (
    "A 16-bit pixel art sprite sheet showing a row of exactly four different "
    "village characters, standing side by side, evenly spaced, all the same "
    "height and drawn at the same scale, all facing the viewer. "
    "Left to right: (1) a " + CAST[0] + "; (2) a " + CAST[1] + "; (3) a " +
    CAST[2] + "; (4) a " + CAST[3] + ". "
    "Style: chunky retro pixel art with a limited palette of at most 12 colours. "
    "CRITICAL: the silhouette of every character must be completely hard-edged, "
    "with absolutely no anti-aliasing, no soft edges, no semi-transparent pixels, "
    "no blurring and no dithering anywhere, including where the characters meet "
    "the background. "
    "The background must be one flat, uniform, fully saturated magenta colour "
    "(#FF00FF) filling all the space around and between the characters. "
    "Do not draw a frame, a border, dividing lines, panels, text, labels, "
    "numbers, a ground plane, shadows or scenery. The magenta gap between each "
    "character must be wide and completely clean, so that every character is "
    "fully separated from its neighbours."
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
    body = {
        "model": MODEL,
        "modalities": ["image", "text"],
        "messages": [{"role": "user", "content": PROMPT}],
    }
    request = urllib.request.Request(
        API,
        data=json.dumps(body).encode(),
        headers={
            "Authorization": "Bearer " + api_key(),
            "Content-Type": "application/json",
            "HTTP-Referer": "https://localhost/airogue-sprite-preview",
            "X-Title": "AIRogue sprite preview",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=240) as response:
            payload = json.load(response)
    except urllib.error.HTTPError as error:
        sys.exit(f"HTTP {error.code}: {error.read().decode(errors='replace')[:600]}")

    data_url = extract_image(payload)
    if data_url is None:
        message = (payload.get("choices") or [{}])[0].get("message") or {}
        sys.exit(f"no image in response: {json.dumps(message)[:600]}")

    header, encoded = data_url.split(",", 1)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as handle:
        handle.write(base64.b64decode(encoded))
    with open(os.path.join(HERE, "raw", "lineup.prompt.txt"), "w") as handle:
        handle.write(PROMPT + "\n")

    usage = payload.get("usage") or {}
    print(f"wrote {OUT} ({os.path.getsize(OUT)} bytes)")
    print(f"cost: {usage.get('cost')}  usage: {json.dumps(usage.get('completion_tokens_details'))}")


if __name__ == "__main__":
    main()
