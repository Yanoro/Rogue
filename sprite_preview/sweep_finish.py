#!/usr/bin/env python3
"""Sweep the finishing parameters and lay the results out for judgement.

Scratch tooling; not part of the game build.

The finishing stage (finish_props.py) has three numeric knobs -- `tolerance`,
`alpha_gain`, `erode` -- that trade off against each other, and the cost of
getting them wrong is a grey fringe or a chewed-up silhouette. Picking them by
argument is how you end up with both. This runs the same render through a grid
of settings and prints one labelled sheet, so the choice is made by looking.

It is free: no GPU, no network. It operates on renders already on disk.

Usage:
    # default grid: tolerance x alpha_gain
    sprite_preview/run.sh sweep_finish.py sprite_preview/comfy_out/probe/v2_0.png
    # include erosion
    sprite_preview/run.sh sweep_finish.py <render> --erode 0 1 2
"""

import argparse
import itertools
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow missing; use sprite_preview/run.sh")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import finish_props as F  # noqa: E402  (needs the path insert above)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("render")
    ap.add_argument("--tolerance", type=float, nargs="+", default=[30, 38, 46])
    ap.add_argument("--alpha-gain", type=float, nargs="+", default=[1.0, 1.5, 2.5])
    ap.add_argument("--erode", type=int, nargs="+", default=[0])
    ap.add_argument("--alpha-cut", type=float, nargs="+", default=[0.0])
    ap.add_argument("--out", default=os.path.join(F.OUT_DIR, "sweep.png"))
    args = ap.parse_args()

    bg = F.game_background()
    rows = []
    for tolerance, gain, erode, cut in itertools.product(
            args.tolerance, args.alpha_gain, args.erode, args.alpha_cut):
        sprite, info = F.finish(args.render, 32, int(tolerance),
                                Image.LANCZOS, None, 0.06, False, gain, erode,
                                cut)
        label = f"t{int(tolerance)} g{gain} e{erode} c{cut}"
        if sprite is None:
            rows.append((label + " REJECT", None))
            print(f"{label}: REJECT {info.get('reason')}")
            continue
        rows.append((label, sprite))
        print(f"{label}: fill {info['sprite_fill']}% "
              f"subject {info['subject_px']}")

    try:
        font = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 12)
    except OSError:
        font = ImageFont.load_default()

    zoom = 7
    tile = 32 * zoom
    pad = 8
    label_h = 14
    cols = len(args.alpha_gain) * len(args.erode) * len(args.alpha_cut)
    width = cols * (tile + pad) + pad
    height = (len(rows) // cols + 1) * (tile + label_h + pad) + pad
    canvas = Image.new("RGB", (width, height), (26, 26, 32))
    draw = ImageDraw.Draw(canvas)

    for index, (label, sprite) in enumerate(rows):
        col = index % cols
        row = index // cols
        x = pad + col * (tile + pad)
        y = pad + row * (tile + label_h + pad)
        if sprite is None:
            draw.text((x, y + tile // 2), "REJECT", fill=(255, 90, 90), font=font)
            continue
        flat = F.on_game_background(sprite, bg)
        canvas.paste(flat.resize((tile, tile), Image.NEAREST), (x, y))
        draw.text((x, y + tile + 1), label, fill=(225, 225, 230), font=font)

    canvas.save(args.out)
    print(f"wrote {os.path.relpath(args.out, ROOT)}")


if __name__ == "__main__":
    main()
