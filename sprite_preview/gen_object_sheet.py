#!/usr/bin/env python3
"""Generate ALL object sprites in a single image, as a labelled grid.

Scratch tooling; not part of the game build.

Why one image: the provider bills 1,290 image tokens per 1024x1024 *output*,
regardless of how many subjects it contains. Measured, not assumed -- a single
farmer and a four-character lineup both came back at 1,290 image tokens and
$0.0388. So 19 objects in one sheet cost the same as 1 object alone, which is a
19x saving over one request per object.

Why a reference image: one sheet is internally consistent by construction, but it
still has to match the four agent sprites that already exist. The farmer sprite is
passed as an input image and referenced as a *style* anchor only -- outline
weight, palette family, shading, scale -- so the objects look like they belong in
the same set.

Usage:  python3 sprite_preview/gen_object_sheet.py [--no-reference]
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
OUT = os.path.join(HERE, "raw", "objects_sheet.png")
REFERENCE = os.path.join(ROOT, "data", "sprites", "farmer.png")

# Reading order, left to right, top to bottom, in rows of five. The order is the
# contract that makes the sheet sliceable: cell N holds OBJECTS[N]. The model is
# asked for a 5x4 grid so the last row has one empty cell rather than the layout
# floating loose.
OBJECTS = [
    "an apple: round, red, with a small green leaf and a short brown stem",
    "a loaf of bread: round and brown, with a lighter slash across the top",
    "a small stack of round bronze coins",
    "a tied white cloth sack of flour",
    "a grey metal ingot bar",
    "a rough dark grey rock with orange-brown iron ore flecks in it",
    "a farming scythe: a long wooden handle with a curved grey blade at the top",
    "a bundle of golden wheat stalks, heads of grain at the top",
    "a small pile of golden grain seed",
    "freshly planted soil: a small dark tilled earth mound with a few seeds",
    "a tiny green sprout with two leaves growing out of dark soil",
    "a tall ripe wheat plant: golden stalks heavy with grain",
    "a leafy green tree with red apples in its branches",
    "a grey rock outcrop with chunks of iron ore embedded in it",
    "a black iron blacksmith's anvil standing on a small wooden stump",
    "a wooden treasure chest with dark iron bands and a latch",
    "a windmill: a stone tower with four large wooden sails",
    "a stone bread oven with a dark arched opening and a chimney",
    "a stone smelter furnace with a glowing orange opening and a chimney",
]

STYLE = (
    "A 16-bit pixel art sprite sheet of individual game objects, arranged in a "
    "strict grid of 5 columns by 4 rows of equally sized cells. Place the "
    "objects in reading order, left to right then top to bottom, one object "
    "centred in its own cell: "
    + "; ".join(f"{index + 1}) {item}" for index, item in enumerate(OBJECTS))
    + ". The last cell of the final row is left completely empty. "
    "Every object must be drawn at the same scale as the others, isolated on its "
    "own, with no overlap between cells. "
    "STYLE: chunky retro pixel art with a limited palette of at most 12 colours, "
    "matching the art style of the attached reference sprite exactly -- the same "
    "outline weight, the same kind of limited palette, the same simple flat "
    "shading, and the same level of detail. The attached image is a style "
    "reference only: do not include that character in the sheet. "
    "CRITICAL: every silhouette must be completely hard-edged, with absolutely no "
    "anti-aliasing, no soft edges, no semi-transparent pixels, no blurring and no "
    "dithering, including where the objects meet the background. "
    "The background must be one flat, uniform, fully saturated magenta colour "
    "(#FF00FF) filling all space around and between the objects. "
    "Do not draw a frame, a border, grid lines, panels, text, labels, letters, "
    "numbers, a ground plane, drop shadows, pedestals or scenery. No object may "
    "cast a shadow or sit on a base."
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
    use_reference = "--no-reference" not in sys.argv
    content = [{"type": "text", "text": STYLE}]
    if use_reference and os.path.exists(REFERENCE):
        with open(REFERENCE, "rb") as handle:
            encoded = base64.b64encode(handle.read()).decode()
        content.append(
            {
                "type": "image_url",
                "image_url": {"url": "data:image/png;base64," + encoded},
            }
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
            "X-Title": "AIRogue object sprite sheet",
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

    header, encoded = data_url.split(",", 1)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as handle:
        handle.write(base64.b64decode(encoded))
    with open(os.path.join(HERE, "raw", "objects_sheet.prompt.txt"), "w") as handle:
        handle.write(STYLE + "\n")

    usage = payload.get("usage") or {}
    print(f"wrote {OUT} ({os.path.getsize(OUT)} bytes)")
    print(f"cost: {usage.get('cost')}")
    print(f"prompt_tokens: {usage.get('prompt_tokens')}  "
          f"image_tokens: {(usage.get('completion_tokens_details') or {}).get('image_tokens')}")


if __name__ == "__main__":
    main()
