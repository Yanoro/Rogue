#!/usr/bin/env python3
"""Judge generated prop sprites the way the game will show them.

Scratch tooling; not part of the game build.

The only review that counts is the sprite at 32px in the game next to real art
(`tools/README.md` §4). A 512px render always looks better than it will read at
32px, so this tool never shows a full-size render on its own -- it shows the
downscaled sprite, enlarged with NEAREST so the pixels are honest, and it shows
the real `data/sprites/` props in the same grid as the yardstick.

Two jobs:

  `--sheet`   a labelled contact sheet: each candidate at 8x, on the same
              background, next to the real props it has to sit beside.
  `--analyze` numbers per image: colour count, subject bounding box, the flat
              background colour actually detected, and the fraction of the frame
              the subject fills. Numbers catch the failures the eye forgives --
              a sprite that is 92% frame is cropping badly, and one with 3000
              colours did not get the palette lock.

Palette lock
------------
`data/palette.json` is 128 colours, `derivedWith: "median-cut over opaque pixels"`
from the real sprite set, and is already 8-bit RGB. Locking a candidate to it and
to the real sprites' background is the cheapest way to make a batch look like one
hand drew it, and it is what makes `--analyze` colour counts comparable across
runs.

Usage (Pillow comes from nix-shell, see run.sh):

    python3 sprite_preview/comfy_sheet.py --sheet
    python3 sprite_preview/comfy_sheet.py --analyze
    python3 sprite_preview/comfy_sheet.py --sheet --lock --out /tmp/locked.png
"""

import argparse
import json
import os
import sys
from collections import Counter

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit(
        "Pillow is not importable. Use the wrapper, which puts it on the path:\n"
        "    sprite_preview/run.sh comfy_sheet.py --sheet"
    )

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_DIR = os.path.join(HERE, "comfy_out")
GAME_SPRITES = os.path.join(ROOT, "data", "sprites")
PALETTE_PATH = os.path.join(ROOT, "data", "palette.json")

# Props that must never be shipped again: this batch is world objects, not the
# agent characters, and a prop sheet that accidentally includes a character is a
# sign the prompt drifted.
TILE_PREFIX = "tile_"

# Review scale. 8x is the scale gen_object_sheet.py used, and it is the smallest
# that still reads on a 32px sprite without a magnifier.
ZOOM = 8


def load_palette():
    with open(PALETTE_PATH) as handle:
        data = json.load(handle)
    return [tuple(c) for c in data["colors"]]


def background_of(img):
    """The flat colour around the subject: the most common colour in the border.

    Sampling the border rather than the whole image is deliberate. The most common
    colour overall is sometimes a large flat area *inside* the subject (a barrel's
    lid, an apron), and keying that out would delete part of the object.
    """
    w, h = img.size
    px = img.convert("RGB").load()
    border = []
    # 2px in from each edge: far enough in to clear any 1px outline artefact,
    # close enough out that the subject rarely reaches it.
    inset = 2
    for x in range(0, w, max(1, w // 64)):
        for y in (inset, inset + 1, h - inset - 1, h - inset):
            border.append(px[x, y])
    for y in range(0, h, max(1, h // 64)):
        for x in (inset, inset + 1, w - inset - 1, w - inset):
            border.append(px[x, y])
    return Counter(border).most_common(1)[0][0]


def subject_bbox(img, bg, tolerance=32):
    """Bounding box of everything that is not the background colour.

    A tolerance is needed because a diffusion render never produces one flat
    colour: the "flat" green background in the first live barrel came back as
    85218 distinct colours overall, with the background alone spanning several
    values around (50,174,34).
    """
    w, h = img.size
    px = img.convert("RGB").load()
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if (abs(r - bg[0]) > tolerance or abs(g - bg[1]) > tolerance
                    or abs(b - bg[2]) > tolerance):
                if x < minx:
                    minx = x
                if y < miny:
                    miny = y
                if x > maxx:
                    maxx = x
                if y > maxy:
                    maxy = y
    if maxx < 0:
        return None
    return (minx, miny, maxx, maxy)


def lock_to_palette(img, palette, bg=None, dither=False):
    """Quantise to the game palette, mapping transparency to the game's key.

    `Image.quantize` with an explicit palette and NEAREST-style mapping, then
    converted back to RGBA. Quantising AFTER the downscale (not before) is the
    rule from tools/README.md §5: quantise the small image, or the downscale
    re-introduces colours the palette does not have.
    """
    rgb = img.convert("RGB")
    # Build a palette image: the game colours, padded to 256 entries.
    pal_img = Image.new("P", (1, 1))
    flat = []
    for colour in palette:
        flat.extend(colour)
    flat.extend([0, 0, 0] * (256 - len(palette)))
    pal_img.putpalette(flat)
    quantised = rgb.quantize(palette=pal_img, dither=Image.Dither.FLOYDSTEINBERG
                             if dither else Image.Dither.NONE)
    return quantised.convert("RGB")


def downscale(img, size=32):
    """512 -> 32 with NEAREST.

    NEAREST, not LANCZOS: any smooth filter invents intermediate colours and
    soft edges, and the whole point of a 32px sprite is that every pixel is a
    deliberate choice. tools/README.md §5 names BILINEAR as the cause of
    "blurry, not pixel art" for exactly this reason.
    """
    return img.resize((size, size), Image.NEAREST)


def normalize(img, size=32, palette=None):
    """Full candidate pipeline: crop to subject, pad, downscale, optional lock.

    Square-pad rather than stretch, so a tall prop (a lamp post, a ladder) keeps
    its proportions instead of being squashed into a 32x32 box.
    """
    bg = background_of(img)
    box = subject_bbox(img, bg)
    if box:
        # A 6% margin so the subject does not touch the sprite edge.
        minx, miny, maxx, maxy = box
        pad_x = max(1, int((maxx - minx) * 0.06))
        pad_y = max(1, int((maxy - miny) * 0.06))
        minx = max(0, minx - pad_x)
        miny = max(0, miny - pad_y)
        maxx = min(img.width - 1, maxx + pad_x)
        maxy = min(img.height - 1, maxy + pad_y)
        img = img.crop((minx, miny, maxx + 1, maxy + 1))

    # Square canvas, subject centred, background filled with the game's own
    # sprite background if we can find it.
    side = max(img.width, img.height)
    canvas = Image.new("RGB", (side, side), bg)
    canvas.paste(img.convert("RGB"), ((side - img.width) // 2,
                                     (side - img.height) // 2))
    small = downscale(canvas, size)
    if palette:
        small = lock_to_palette(small, palette)
    return small, bg, box


def game_background():
    """The background colour the real sprites use, so candidates match them.

    Read from an actual prop rather than guessed: whatever the corner pixel of
    `data/sprites/anvil.png` is, is what a new prop has to sit on.
    """
    path = os.path.join(GAME_SPRITES, "anvil.png")
    if not os.path.exists(path):
        return None
    with Image.open(path) as img:
        return img.convert("RGB").getpixel((0, 0))


def analyze(paths, palette, size=32):
    print(f"{'file':38s} {'colours':>8s} {'bg':>16s} {'fill%':>6s} {'box':>18s}")
    print("-" * 92)
    for path in paths:
        try:
            with Image.open(path) as raw:
                img = raw.convert("RGB")
                bg = background_of(img)
                box = subject_bbox(img, bg)
                fill = 0.0
                if box:
                    area = (box[2] - box[0] + 1) * (box[3] - box[1] + 1)
                    fill = 100.0 * area / (img.width * img.height)
                colours = len(Counter(img.getdata()))
        except Exception as exc:  # a broken PNG should not stop the batch
            print(f"{os.path.basename(path):38s} ERROR {exc}")
            continue
        name = os.path.relpath(path, ROOT)
        box_s = f"{box}" if box else "none"
        print(f"{name[-38:]:38s} {colours:8d} {str(bg):>16s} {fill:6.1f} {box_s:>18s}")


def sheet(paths, palette=None, out=None, size=32, cols=4, lock=False):
    bg = game_background() or (40, 40, 48)
    try:
        font = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 13
        )
        font_small = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 11
        )
    except OSError:
        font = ImageFont.load_default()
        font_small = font

    tile = size * ZOOM
    label_h = 20
    pad = 10
    rows = (len(paths) + cols - 1) // cols
    width = cols * (tile + pad) + pad
    height = rows * (tile + label_h + pad) + pad
    canvas = Image.new("RGB", (width, height), (28, 28, 34))
    draw = ImageDraw.Draw(canvas)

    for index, path in enumerate(paths):
        col = index % cols
        row = index // cols
        x = pad + col * (tile + pad)
        y = pad + row * (tile + label_h + pad)
        try:
            with Image.open(path) as raw:
                small, detected_bg, _ = normalize(
                    raw.convert("RGB"), size, palette if lock else None
                )
        except Exception as exc:
            draw.text((x, y), f"ERROR {exc}", fill=(255, 80, 80), font=font_small)
            continue

        # Checkerboard behind the sprite so a transparent or wrongly-keyed
        # background is visible rather than blending into the sheet.
        for cy in range(0, tile, 16):
            for cx in range(0, tile, 16):
                shade = (58, 58, 66) if ((cx // 16 + cy // 16) % 2) else (44, 44, 52)
                draw.rectangle([x + cx, y + cy, x + cx + 15, y + cy + 15],
                               fill=shade)
        enlarged = small.resize((tile, tile), Image.NEAREST)
        canvas.paste(enlarged, (x, y))

        stem = os.path.splitext(os.path.basename(path))[0]
        draw.text((x, y + tile + 3), stem[:34], fill=(230, 230, 235), font=font)
        draw.text((x, y + tile + 3 + 14), f"bg{detected_bg}", fill=(150, 150, 160),
                  font=font_small)

    out = out or os.path.join(OUT_DIR, "contact_sheet.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    canvas.save(out)
    print(f"wrote {os.path.relpath(out, ROOT)}  ({canvas.width}x{canvas.height})")
    return out


def collect(explicit):
    if explicit:
        return explicit
    if not os.path.isdir(OUT_DIR):
        return []
    found = []
    for name in sorted(os.listdir(OUT_DIR)):
        if name.lower().endswith(".png"):
            found.append(os.path.join(OUT_DIR, name))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("images", nargs="*", help="default: everything in comfy_out/")
    ap.add_argument("--sheet", action="store_true", help="labelled contact sheet")
    ap.add_argument("--analyze", action="store_true", help="print per-image stats")
    ap.add_argument("--lock", action="store_true",
                    help="quantise to data/palette.json before showing")
    ap.add_argument("--size", type=int, default=32)
    ap.add_argument("--cols", type=int, default=4)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    paths = collect(args.images)
    if not paths:
        sys.exit(f"no images found in {OUT_DIR}. Generate some first.")

    palette = load_palette() if (args.lock or args.analyze) else None

    if not (args.sheet or args.analyze):
        args.sheet = True

    if args.analyze:
        analyze(paths, palette, args.size)
        print()
    if args.sheet:
        sheet(paths, palette, args.out, args.size, args.cols, args.lock)


if __name__ == "__main__":
    main()
