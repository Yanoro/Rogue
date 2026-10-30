#!/usr/bin/env python3
"""Put a candidate and the real art pack side by side, pixel for pixel.

Scratch tooling; not part of the game build.

`comfy_sheet.py` shows candidates alone. That answers "is this a barrel" but not
"does this belong in the same game as `data/sprites/apple.png`". This tool
answers the second question, which is the one that decides whether a batch is
usable (`tools/README.md` §4: "does it look like the same artist made it").

How it compares
---------------
It does not eyeball. For each sprite it reports the two numbers that separate
hand-authored pixel art from a downscaled diffusion render:

  * **colour count** -- a 32x32 hand-authored prop is typically tens of colours.
    A downscaled render is thousands, because every destination pixel mixes its
    neighbours. If a candidate has 10x the colours of `apple.png`, it is a
    smooth image wearing a 32x32 costume and will read as mush.
  * **edge hardness** -- the share of neighbouring pixel pairs that are exactly
    equal. Hard pixel art has long flat runs and sharp single-pixel steps; a
    downscaled render dithers and almost never repeats exactly.

Output is one PNG: the real sprites in a labelled row above the candidates, all
at the same zoom, so the comparison is like for like.

Usage:
    sprite_preview/run.sh compare_real.py sprite_preview/comfy_out/barrel_0.png
    sprite_preview/run.sh compare_real.py --real apple bread anvil barrel_0
"""

import argparse
import os
import sys
from collections import Counter

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow missing; use sprite_preview/run.sh")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GAME = os.path.join(ROOT, "data", "sprites")
OUT = os.path.join(HERE, "comfy_out")
ZOOM = 10


def stats(img):
    """Colour count and exact-repeat rate for a small RGBA/RGB sprite."""
    img = img.convert("RGB")
    w, h = img.size
    px = img.load()
    colours = Counter(img.getdata())
    same = 0
    total = 0
    for y in range(h):
        for x in range(w):
            if x + 1 < w:
                total += 1
                if px[x, y] == px[x + 1, y]:
                    same += 1
            if y + 1 < h:
                total += 1
                if px[x, y] == px[x, y + 1]:
                    same += 1
    return len(colours), 100.0 * same / max(1, total), colours.most_common(3)


def describe(label, img):
    """Print the two numbers that separate pixel art from a downscaled render.

    Colour count alone is ambiguous on its own: `apple.png` has 382 colours and
    is unmistakably hand-authored. The pair that discriminates is
    (colour count, exact-repeat rate) -- the real sprites sit near 350-380
    colours at ~45-55% exact repeats, i.e. many colours but large flat areas.
    A downscaled render lands at a similar colour count with a far lower repeat
    rate, which is the signature of dithering rather than deliberate flat fills.
    """
    img = img.convert("RGB")
    colours, repeat, _ = stats(img)
    print(f"{label:28s} colours {colours:6d}   exact-repeat {repeat:5.1f}%")
    return colours


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("candidates", nargs="*", help="paths or stems in comfy_out/")
    ap.add_argument("--real", nargs="*", default=["apple", "bread", "anvil_tree"],
                    help="stems in data/sprites/ to use as the yardstick")
    ap.add_argument("--out", default=os.path.join(OUT, "compare_real.png"))
    args = ap.parse_args()

    def resolve(item):
        if os.path.exists(item):
            return item
        guess = os.path.join(OUT, item if item.endswith(".png") else item + ".png")
        if os.path.exists(guess):
            return guess
        guess = os.path.join(GAME, item if item.endswith(".png") else item + ".png")
        if os.path.exists(guess):
            return guess
        return None

    # Default the yardstick to props that actually exist.
    real_stems = [s for s in args.real if os.path.exists(os.path.join(GAME, s + ".png"))]
    if not real_stems:
        real_stems = [s for s in ["apple", "bread", "anvil", "chest", "mill"]
                      if os.path.exists(os.path.join(GAME, s + ".png"))]

    reals = [os.path.join(GAME, s + ".png") for s in real_stems]
    cands = [p for p in (resolve(c) for c in args.candidates) if p]

    if not cands:
        sys.exit("no candidates resolved. Give paths or stems in comfy_out/.")

    print("=== real art pack (the target) ===")
    for path in reals:
        with Image.open(path) as img:
            describe("real/" + os.path.basename(path), img)

    print("\n=== candidates ===")
    rows = []
    for path in cands:
        with Image.open(path) as raw:
            img = raw.convert("RGB")
            small = img.resize((32, 32), Image.LANCZOS)
            describe(os.path.basename(path) + " (lanczos32)", small)
            rows.append((os.path.basename(path), small, img.resize((32, 32), Image.NEAREST)))

    # Build the comparison image.
    try:
        font = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 12)
    except OSError:
        font = ImageFont.load_default()

    tile = 32 * ZOOM
    pad = 12
    label_h = 18
    # Load the real sprites as images here rather than passing paths into the
    # render loop; the --real filter above already guarantees they exist.
    real_imgs = []
    for path in reals:
        with Image.open(path) as raw:
            real_imgs.append(raw.convert("RGB"))
    group = real_imgs + [r[1] for r in rows]
    labels = ([os.path.basename(p) for p in reals]
              + [r[0] + " L32" for r in rows])
    cols = max(len(group), 1)
    width = cols * (tile + pad) + pad
    height = tile + label_h + 2 * pad + 8
    canvas = Image.new("RGB", (width, height), (26, 26, 32))
    draw = ImageDraw.Draw(canvas)

    for i, (img, label) in enumerate(zip(group, labels)):
        x = pad + i * (tile + pad)
        y = pad
        canvas.paste(img.resize((tile, tile), Image.NEAREST), (x, y))
        draw.text((x, y + tile + 2), label[:22], fill=(230, 230, 235), font=font)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    canvas.save(args.out)
    print(f"\nwrote {os.path.relpath(args.out, ROOT)}")


if __name__ == "__main__":
    main()
