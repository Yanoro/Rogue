#!/usr/bin/env python3
"""Compare several finished sprite sets side by side, one row per set.

Scratch tooling; not part of the game build.

`comfy_sheet.py` shows one batch. When the question is "which *pipeline* is
better" -- 512px vs 768px generation, soft matte vs hard edge, palette-locked vs
not -- the comparison has to be across sets, on identical subjects, at the same
zoom. Reading two sheets from memory and deciding is how a worse option wins.

The real art pack is always the top row, so the target is in view the whole time.

Usage:
    sprite_preview/run.sh compare_sets.py \
        --set raw512=sprite_preview/comfy_out/final \
        --set gen768=sprite_preview/comfy_out/final768
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow missing; use sprite_preview/run.sh")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GAME = os.path.join(ROOT, "data", "sprites")


def load_any(path):
    with Image.open(path) as img:
        return img.convert("RGBA")


def find(directory, stem):
    """Locate <stem>.png or <stem>_0.png, tolerating the variant suffix."""
    for candidate in (stem + ".png", stem + "_0.png"):
        path = os.path.join(directory, candidate)
        if os.path.exists(path):
            return path
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--set", action="append", default=[],
                    metavar="LABEL=DIR", help="a labelled directory (repeatable)")
    ap.add_argument("--subjects", nargs="*", default=None,
                    help="stems to compare; default is the union across sets")
    ap.add_argument("--real", nargs="*", default=["apple", "bread", "anvil", "chest"])
    ap.add_argument("--out", default=os.path.join(HERE, "comfy_out", "compare_sets.png"))
    ap.add_argument("--zoom", type=int, default=6)
    args = ap.parse_args()

    sets = []
    for spec in args.set:
        if "=" not in spec:
            sys.exit(f"--set needs LABEL=DIR, got {spec!r}")
        label, directory = spec.split("=", 1)
        if not os.path.isdir(directory):
            sys.exit(f"no such directory: {directory}")
        sets.append((label, directory))
    if not sets:
        sys.exit("give at least one --set LABEL=DIR")

    # Subjects: explicit, else the union of every set's contents.
    if args.subjects:
        subjects = args.subjects
    else:
        seen = []
        for _, directory in sets:
            for name in sorted(os.listdir(directory)):
                if not name.endswith(".png") or name.startswith("_"):
                    continue
                stem = name[:-4]
                # Group variants under their prop name.
                base = stem.rsplit("_", 1)[0] if stem.rsplit("_", 1)[-1].isdigit() \
                    else stem
                if base not in seen:
                    seen.append(base)
        subjects = seen

    if not subjects:
        sys.exit("no subjects to compare")

    try:
        font = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 12)
        row_font = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 13)
    except OSError:
        font = ImageFont.load_default()
        row_font = font

    real_imgs = []
    for stem in args.real:
        path = os.path.join(GAME, stem + ".png")
        if os.path.exists(path):
            real_imgs.append((stem, load_any(path)))

    rows = [("real art pack", real_imgs)]
    for label, directory in sets:
        imgs = []
        for stem in subjects:
            path = find(directory, stem)
            imgs.append((stem, load_any(path) if path else None))
        rows.append((label, imgs))

    cols = len(subjects)
    tile = 32 * args.zoom
    pad = 8
    label_h = 16
    row_label_w = 130
    width = row_label_w + cols * (tile + pad) + pad
    height = len(rows) * (tile + label_h + pad) + pad
    canvas = Image.new("RGB", (width, height), (26, 26, 32))
    draw = ImageDraw.Draw(canvas)

    for r, (label, imgs) in enumerate(rows):
        y = pad + r * (tile + label_h + pad)
        draw.text((pad, y + tile // 2 - 8), label[:18], fill=(240, 220, 150),
                  font=row_font)
        # Real pack has no per-column label; the sets do.
        for c, (stem, img) in enumerate(imgs):
            x = row_label_w + pad + c * (tile + pad)
            if img is None:
                draw.text((x + 6, y + tile // 2), "missing", fill=(255, 90, 90),
                          font=font)
                continue
            flat = Image.new("RGB", (32, 32), (24, 20, 28))
            flat.paste(img, (0, 0), img)
            canvas.paste(flat.resize((tile, tile), Image.NEAREST), (x, y))
            if r == 0:
                draw.text((x, y + tile + 1), stem[:18], fill=(200, 200, 210),
                          font=font)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    canvas.save(args.out)
    print(f"wrote {os.path.relpath(args.out, ROOT)}  ({canvas.width}x{canvas.height})")


if __name__ == "__main__":
    main()
