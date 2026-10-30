#!/usr/bin/env python3
"""Measure how much real detail a render actually contains.

Scratch tooling; not part of the game build.

Why this exists
---------------
A diffusion render at 512px is not a 512px sprite. When the pixel-art LoRA is
driving, the model paints *scaled-up* pixel art: a grid of large uniform blocks,
often 8-16px across, which is a native image of only 32-64 pixels wide. That
distinction decides the entire downscale strategy:

  * a render whose native grid is ~32px wide cannot be downscaled to a 32x32
    sprite at all -- it is already at that size, and resizing only destroys it;
  * a render whose native grid is ~512px (i.e. genuinely per-pixel detail) has
    to be downscaled, and that is the case where NEAREST + palette lock works.

Guessing this by eye is unreliable because a viewer upscales whatever it is
given. Measured run-lengths are not.

Method: walk rows and columns and histogram the lengths of runs of *exactly
equal* colour. Smooth shading produces short runs; scaled-up pixel art produces
a strong spike at the block size. Also reports the mode of the run lengths among
runs longer than one pixel, which is the block size when a spike exists.

Usage:
    sprite_preview/run.sh measure_blocks.py sprite_preview/comfy_out/*.png
"""

import sys
from collections import Counter

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow missing; use sprite_preview/run.sh")


def run_lengths(img, step=7):
    """Histogram of equal-colour run lengths, sampled every `step` rows/cols."""
    w, h = img.size
    px = img.load()
    runs = Counter()

    for y in range(0, h, step):
        run = 1
        prev = px[0, y]
        for x in range(1, w):
            c = px[x, y]
            if c == prev:
                run += 1
            else:
                runs[run] += 1
                run = 1
                prev = c
        runs[run] += 1

    for x in range(0, w, step):
        run = 1
        prev = px[x, 0]
        for y in range(1, h):
            c = px[x, y]
            if c == prev:
                run += 1
            else:
                runs[run] += 1
                run = 1
                prev = c
        runs[run] += 1

    return runs


def report(path):
    with Image.open(path) as raw:
        img = raw.convert("RGB")
        w, h = img.size
        runs = run_lengths(img)

    mult = {length: count for length, count in runs.items() if length > 1}
    total_multi = sum(mult.values())
    # Block size candidate: the most common run length above 1. If the image is
    # genuinely smooth this is just "2" and means nothing -- hence also
    # reporting the coverage of the longest common runs.
    top = sorted(mult.items(), key=lambda kv: -kv[1])[:6]
    strong = [(length, count) for length, count in top
              if count > 0.05 * total_multi]

    print(f"{path}")
    print(f"  canvas {w}x{h}   single-pixel runs (smooth detail): "
          f"{runs.get(1, 0)}")
    print(f"  multi-pixel runs: {total_multi}")
    for length, count in top:
        share = 100.0 * count / max(1, total_multi)
        print(f"    run {length:4d}px  x{count:<7d} {share:5.1f}%")
    if strong:
        block = strong[0][0]
        est = round(w / block)
        print(f"  => dominant block ~{block}px  => native pixel grid "
              f"~{est}x{round(h / block)}")
    else:
        print("  => no dominant block; render is smooth, not scaled pixel art")
    print()


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        try:
            report(path)
        except Exception as exc:
            print(f"{path}: ERROR {exc}\n")


if __name__ == "__main__":
    main()
