#!/usr/bin/env python3
"""Generate agent sprites with an OpenRouter image model.

Scratch tooling for evaluating AI-generated sprites. NOT part of the game build
and not referenced by CMakeLists.txt. Writes raw model output to
sprite_preview/raw/<id>.png; postprocess.py turns those into game-sized PNGs.

Usage:  python3 sprite_preview/gen_sprite.py farmer miller smith trader
Reads the API key from model.json (never printed, never written to disk here).
"""

import base64
import json
import os
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RAW = os.path.join(HERE, "raw")

MODEL = "google/gemini-2.5-flash-image"
API = "https://openrouter.ai/api/v1/chat/completions"

# Identical boilerplate for every subject: this is the only lever keeping a
# separately generated set stylistically consistent, so it is deliberately
# verbose and repeated verbatim per request.
STYLE = (
    "A 16-bit pixel art game sprite of a single character, full body, standing, "
    "facing the viewer, centred and filling most of the frame. Chunky readable "
    "pixels, a limited palette of at most 12 colours, hard edges, no "
    "anti-aliasing. The background must be one completely flat, uniform, "
    "saturated magenta colour (#FF00FF), with no gradient, no shadow, no ground "
    "plane, no scenery and no lighting effects. Do not draw a frame, border, "
    "text, label, letter, number, watermark or grid, and do not divide the image "
    "into panels. Exactly one character, viewed from the front."
)

SUBJECTS = {
    "farmer": (
        "A village farmer: weathered face, a wide-brimmed straw hat, a rolled-up "
        "beige tunic, brown trousers, muddy boots, and a wooden pitchfork held "
        "upright in one hand."
    ),
    "miller": (
        "A village miller: flour-dusted white apron over a brown tunic, a soft "
        "dust cap, rolled sleeves, and a tied flour sack resting on one shoulder."
    ),
    "smith": (
        "A village blacksmith: a heavy leather apron, bare muscular arms, dark "
        "soot-stained trousers, and a blacksmith's hammer in one hand."
    ),
    "trader": (
        "A travelling trader: a colourful layered cloak, a wide felt hat, a "
        "leather satchel across the chest, and a small coin pouch held in one "
        "hand."
    ),
}


def api_key():
    with open(os.path.join(ROOT, "model.json")) as handle:
        config = json.load(handle)
    key = config.get("apiKey") or ""
    if not key:
        sys.exit("model.json has no apiKey")
    return key


def extract_image(payload):
    """Find the first data-URL image anywhere in the response.

    The response shape is not worth trusting to one path: some providers put
    images in message.images, some in a content array, so this walks the whole
    document looking for a base64 data URL.
    """
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


def generate(sprite_id, subject, key):
    body = {
        "model": MODEL,
        "modalities": ["image", "text"],
        "messages": [
            {"role": "user", "content": STYLE + " Subject: " + subject}
        ],
    }
    request = urllib.request.Request(
        API,
        data=json.dumps(body).encode(),
        headers={
            "Authorization": "Bearer " + key,
            "Content-Type": "application/json",
            "HTTP-Referer": "https://localhost/airogue-sprite-preview",
            "X-Title": "AIRogue sprite preview",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=180) as response:
            payload = json.load(response)
    except urllib.error.HTTPError as error:
        detail = error.read().decode(errors="replace")[:600]
        print(f"{sprite_id}: HTTP {error.code}: {detail}")
        return False
    except Exception as error:  # noqa: BLE001 - scratch tool, report and continue
        print(f"{sprite_id}: request failed: {error}")
        return False

    data_url = extract_image(payload)
    if data_url is None:
        # Surface why, verbatim: a refusal or a text-only answer is a real result.
        message = (payload.get("choices") or [{}])[0].get("message") or {}
        print(f"{sprite_id}: no image in response. message={json.dumps(message)[:600]}")
        return False

    header, encoded = data_url.split(",", 1)
    extension = "png" if "png" in header else "jpg"
    os.makedirs(RAW, exist_ok=True)
    path = os.path.join(RAW, f"{sprite_id}.{extension}")
    with open(path, "wb") as handle:
        handle.write(base64.b64decode(encoded))
    with open(os.path.join(RAW, f"{sprite_id}.prompt.txt"), "w") as handle:
        handle.write(STYLE + " Subject: " + subject + "\n")

    usage = payload.get("usage") or {}
    print(f"{sprite_id}: wrote {path} ({os.path.getsize(path)} bytes) usage={usage}")
    return True


def main():
    wanted = sys.argv[1:] or list(SUBJECTS)
    unknown = [name for name in wanted if name not in SUBJECTS]
    if unknown:
        sys.exit(f"unknown subjects: {unknown}; known: {list(SUBJECTS)}")
    key = api_key()
    failures = 0
    for name in wanted:
        if not generate(name, SUBJECTS[name], key):
            failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
