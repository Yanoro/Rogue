#!/usr/bin/env python3
"""Promote chosen variants into a curated sprite set, and review it.

Scratch tooling; not part of the game build.

Generation is a *proposal*, not a result. A batch of 24 renders might contain 14
that pass the numeric gate in `finish_props.py` and 5 that a human would actually
ship. This tool records that human decision in a plain file -- which variant of
which prop became the sprite -- so the set is reproducible instead of being "the
files that happened to be in final/ on Tuesday".

The mapping lives in `curated.json` next to this tool. It is a hand-edited file
and that is the point: the judgement is the expensive part, so it is stored rather
than re-derived.

Then it reviews the result the only way that matters -- the curated sprite beside
the real art pack, with the two numbers from `compare_real.py`.

Usage:
    sprite_preview/run.sh curate.py --list                 # what is available
    sprite_preview/run.sh curate.py                        # build from curated.json
    sprite_preview/run.sh curate.py --setup                # write a starter mapping
"""

import argparse
import json
import os
import shutil
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow missing; use sprite_preview/run.sh")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_DIR = os.path.join(HERE, "comfy_out")
CURATED_MAP = os.path.join(HERE, "curated.json")
DEST_DIR = os.path.join(OUT_DIR, "curated")
GAME = os.path.join(ROOT, "data", "sprites")

# Directories holding finished 32x32 sprites, searched in this order when a
# mapping names a bare variant stem. Order matters: earlier wins.
# Only finisher runs tagged with a known provenance belong here. f_v3 and
# f_aziib are the AziibPixelMix checkpoint; final512 is the base + LoRA set. Do
# not add the untagged legacy `final` directory: its contents were produced before
# the is_finished_sprite guard existed and include output that fed itself.
SEARCH_DIRS = ("f_final256", "f_q256", "f_v3", "f_aziib", "final512")


def stats(img):
    img = img.convert("RGB")
    w, h = img.size
    px = img.load()
    colours = len(set(img.getdata()))
    same = total = 0
    for y in range(h):
        for x in range(w):
            if x + 1 < w:
                total += 1
                same += px[x, y] == px[x + 1, y]
            if y + 1 < h:
                total += 1
                same += px[x, y] == px[x, y + 1]
    return colours, 100.0 * same / max(1, total)


def find_variant(stem):
    for directory in SEARCH_DIRS:
        path = os.path.join(OUT_DIR, directory, stem + ".png")
        if os.path.exists(path):
            return path
    # Fall back to a recursive search so a new experiment dir still works.
    for root, _dirs, files in os.walk(OUT_DIR):
        if stem + ".png" in files:
            return os.path.join(root, stem + ".png")
    return None


def available():
    found = {}
    for directory in SEARCH_DIRS:
        full = os.path.join(OUT_DIR, directory)
        if not os.path.isdir(full):
            continue
        for name in sorted(os.listdir(full)):
            if name.endswith(".png") and not name.startswith("_"):
                found.setdefault(directory, []).append(name[:-4])
    return found


def setup_starter():
    """Write a starter mapping naming the first variant of every prop found.

    Explicitly a starting point: it chooses nothing, it just lists what exists so
    the file to edit is in front of you. A mapping that silently picked "the best"
    would be worse than useless, because there is no metric for best.
    """
    mapping = {}
    for _directory, stems in available().items():
        for stem in stems:
            prop = stem.rsplit("_", 1)[0]
            mapping.setdefault(prop, stem)
    with open(CURATED_MAP, "w") as handle:
        json.dump({
            "_comment": "prop -> finished variant stem. Edit by hand: this is the "
                        "record of which render you decided to ship.",
            "sprites": mapping,
        }, handle, indent=2, sort_keys=True)
    return CURATED_MAP


def build(mapping, dest=DEST_DIR, check_real=True):
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)

    if check_real:
        # A curated set that shadows a real sprite is almost always a mistake.
        # Generated art must not silently replace hand-authored art.
        clash = [p for p in mapping
                 if os.path.exists(os.path.join(GAME, p + ".png"))]
        if clash:
            print(f"WARNING: {len(clash)} curated name(s) already exist in "
                  f"data/sprites/ and would shadow real art:")
            print("   " + ", ".join(sorted(clash)))

    rows = []
    missing = []
    for prop in sorted(mapping):
        source = find_variant(mapping[prop])
        if not source:
            missing.append(prop)
            continue
        target = os.path.join(dest, prop + ".png")
        shutil.copyfile(source, target)
        rows.append((prop, target))

    if missing:
        print(f"could not resolve {len(missing)} prop(s): {', '.join(missing)}")
    print(f"curated {len(rows)} sprite(s) into {os.path.relpath(dest, ROOT)}")
    return rows


def review(rows, out):
    try:
        font = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 12)
        small = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 10)
    except OSError:
        font = small = ImageFont.load_default()

    print(f"\n{'curated sprite':26s} {'colours':>8s} {'repeat%':>8s}   "
          f"{'real counterpart':26s} {'colours':>8s} {'repeat%':>8s}")
    print("-" * 96)
    real_cache = {}
    for prop, path in rows:
        with Image.open(path) as img:
            c, r = stats(img.convert("RGBA"))
        real_path = os.path.join(GAME, prop + ".png")
        if os.path.exists(real_path):
            if prop not in real_cache:
                with Image.open(real_path) as img:
                    real_cache[prop] = stats(img.convert("RGBA"))
            rc, rr = real_cache[prop]
            real_s = f"data/sprites/{prop}.png"
            print(f"{prop:26s} {c:8d} {r:8.1f}   {real_s:26s} {rc:8d} {rr:8.1f}")
        else:
            print(f"{prop:26s} {c:8d} {r:8.1f}   {'(no real sprite)':26s} "
                  f"{'-':>8s} {'-':>8s}")

    zoom = 6
    tile = 32 * zoom
    pad = 8
    label_h = 15
    cols = min(5, max(1, len(rows)))
    rws = (len(rows) + cols - 1) // cols
    width = cols * (tile + pad) + pad
    height = rws * (tile + label_h + pad) + pad + 16
    canvas = Image.new("RGB", (width, height), (26, 26, 32))
    draw = ImageDraw.Draw(canvas)
    draw.text((pad, 3), "curated sprites (top) vs real art pack where a "
              "counterpart exists", fill=(160, 160, 175), font=small)
    for index, (prop, path) in enumerate(rows):
        col = index % cols
        row = index // cols
        x = pad + col * (tile + pad)
        y = pad + 16 + row * (tile + label_h + pad)
        with Image.open(path) as img:
            sprite = img.convert("RGBA")
        flat = Image.new("RGB", (32, 32), (24, 20, 28))
        flat.paste(sprite, (0, 0), sprite)
        canvas.paste(flat.resize((tile, tile), Image.NEAREST), (x, y))
        draw.text((x, y + tile + 1), prop[:18], fill=(230, 230, 235), font=font)
    canvas.save(out)
    print(f"\nwrote {os.path.relpath(out, ROOT)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--setup", action="store_true",
                    help="write a starter curated.json from what exists")
    ap.add_argument("--list", action="store_true", help="list available variants")
    ap.add_argument("--out", default=os.path.join(OUT_DIR, "curated_sheet.png"))
    args = ap.parse_args()

    if args.list:
        for directory, stems in available().items():
            print(f"{directory}:")
            for stem in stems:
                print(f"   {stem}")
        return

    if args.setup:
        path = setup_starter()
        print(f"wrote {os.path.relpath(path, ROOT)}")
        print("edit it by hand, then re-run without --setup")
        return

    if not os.path.exists(CURATED_MAP):
        setup_starter()
        print(f"no curated.json existed; wrote a starter at "
              f"{os.path.relpath(CURATED_MAP, ROOT)}")
        print("edit it, then re-run")
        return

    with open(CURATED_MAP) as handle:
        mapping = json.load(handle).get("sprites", {})
    if not mapping:
        sys.exit("curated.json has no 'sprites' mapping")

    rows = build(mapping)
    if rows:
        review(rows, args.out)


if __name__ == "__main__":
    main()
