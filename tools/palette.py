#!/usr/bin/env python3
"""Derive one palette from a set of sprites, and snap sprites to it.

This is the "palette lock" stage of the asset pipeline. See
docs/asset-generation-design.md sections 7.3 and 9.

Why it exists. Every generator in this repo quantises each image to a palette
derived from *that image*: `tools/gen_tile.py` calls
`image.quantize(colors=16, method=MEDIANCUT)`, `slice_object_sheet.py` passes
ImageMagick `-colors 16`, `slice_tile_sheet.py` the same with a larger count. The
result is that two sprites from the same generation run can share no colours at
all. That is the mechanism by which generated art reads as "imported" next to art
that was actually drawn together, and it is invisible in a single sprite -- which
is why the fix is a shared palette rather than a better prompt.

Snapping is also the one stage here that is deterministic. A LoRA drifts and a
model is resampled every run; nearest-colour in a perceptual space does not. So
this is the stage to lean on when the look has to hold.

Three subcommands:

  derive  build a palette from a directory of sprites -> palette.json + palette.png
  lock    snap every sprite in a directory to a palette, into a new directory
  report  measure how far a directory already is from a palette, without writing

Usage:
  python3 tools/palette.py derive --sprites data/sprites --colors 32
  python3 tools/palette.py lock --palette data/palette.json \
      --sprites sprite_preview/tiles_out --out packs/generated/pixel_crawler/sprites
  python3 tools/palette.py report --palette data/palette.json --sprites data/sprites

Pillow is required; on NixOS that means a shell that has it:
  nix-shell -p python3Packages.pillow --run "python3 tools/palette.py ..."
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from collections import Counter

try:
    from PIL import Image
except ImportError:  # pragma: no cover - environment problem, not a code path
    sys.stderr.write(
        "palette.py needs Pillow. On NixOS the system python cannot see it:\n"
        '  nix-shell -p python3Packages.pillow --run "python3 tools/palette.py ..."\n'
    )
    raise SystemExit(2)

# A pixel at or below this alpha is treated as transparent, above it as opaque.
# Pixel art wants binary alpha and the generators produce an anti-aliased rim
# against the chroma-key background, which this removes. `make_sheet.py` measures
# that rim as the share of semi-transparent pixels; this is the stage that
# actually deletes it.
DEFAULT_ALPHA_THRESHOLD = 128


# --------------------------------------------------------------------------
# Colour space
# --------------------------------------------------------------------------


def _srgb_to_linear(channel: int) -> float:
    c = channel / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def rgb_to_oklab(rgb: tuple[int, int, int]) -> tuple[float, float, float]:
    """sRGB 0-255 to OKLab.

    Worth the arithmetic over raw RGB distance because the pack's palette is
    limited and often close together: in RGB, a dark blue and a dark purple can
    be nearer to a mid grey than to each other, and a wrong snap there is a
    visible colour shift on a 32px sprite.
    """
    r, g, b = (_srgb_to_linear(c) for c in rgb)
    l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b
    m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b
    s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b
    l_, m_, s_ = (math.copysign(abs(v) ** (1 / 3), v) for v in (l, m, s))
    return (
        0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
        1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
        0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_,
    )


def nearest_index(
    rgb: tuple[int, int, int],
    palette: list[tuple[int, int, int]],
    palette_lab: list[tuple[float, float, float]],
    cache: dict[tuple[int, int, int], int],
) -> int:
    """Index of the nearest palette entry, memoised.

    Pixel art has few distinct colours per sprite, so the memo turns a per-pixel
    search into a per-colour one and the cost stops depending on sprite area.
    """
    hit = cache.get(rgb)
    if hit is not None:
        return hit
    lab = rgb_to_oklab(rgb)
    best, best_distance = 0, float("inf")
    for i, entry in enumerate(palette_lab):
        dl = lab[0] - entry[0]
        da = lab[1] - entry[1]
        db = lab[2] - entry[2]
        distance = dl * dl + da * da + db * db
        if distance < best_distance:
            best, best_distance = i, distance
    cache[rgb] = best
    return best


# --------------------------------------------------------------------------
# Reading and writing
# --------------------------------------------------------------------------


def sprite_paths(directory: str) -> list[str]:
    if not os.path.isdir(directory):
        raise SystemExit(f"palette.py: no such directory: {directory}")
    names = sorted(n for n in os.listdir(directory) if n.lower().endswith(".png"))
    if not names:
        raise SystemExit(f"palette.py: no .png files in {directory}")
    return [os.path.join(directory, n) for n in names]


def load_palette(path: str) -> list[tuple[int, int, int]]:
    with open(path) as handle:
        doc = json.load(handle)
    colors = doc["colors"] if isinstance(doc, dict) else doc
    palette = [tuple(int(c) for c in entry[:3]) for entry in colors]
    if not palette:
        raise SystemExit(f"palette.py: {path} holds no colours")
    return palette


def save_palette(path: str, palette: list[tuple[int, int, int]], source: str,
                 sprite_count: int, method: str) -> None:
    """Write the palette as JSON (canonical, reviewable) and as a swatch sheet.

    The swatch sheet is not decoration: it is what ImageMagick's `-remap` takes,
    and it lets a human see at a glance whether the palette looks like the pack.
    It contains only palette colours, which is what makes it a valid reference.
    """
    root, _ = os.path.splitext(path)
    with open(root + ".json", "w") as handle:
        json.dump(
            {
                "colors": [list(c) for c in palette],
                "source": source,
                "spriteCount": sprite_count,
                "derivedWith": method,
            },
            handle,
            indent=2,
        )
        handle.write("\n")

    swatch = 16
    per_row = 8
    rows = math.ceil(len(palette) / per_row)
    sheet = Image.new("RGBA", (per_row * swatch, rows * swatch), (0, 0, 0, 0))
    for i, color in enumerate(palette):
        x = (i % per_row) * swatch
        y = (i // per_row) * swatch
        sheet.paste(Image.new("RGBA", (swatch, swatch), color + (255,)), (x, y))
    sheet.save(root + ".png")
    return root


# --------------------------------------------------------------------------
# derive
# --------------------------------------------------------------------------


def derive(sprites_dir: str, colors: int, out: str) -> int:
    paths = sprite_paths(sprites_dir)
    pixels: list[tuple[int, int, int]] = []
    for path in paths:
        sprite = Image.open(path).convert("RGBA")
        for r, g, b, a in sprite.getdata():
            if a >= DEFAULT_ALPHA_THRESHOLD:
                pixels.append((r, g, b))

    if not pixels:
        raise SystemExit(
            f"palette.py: every pixel in {sprites_dir} is transparent; "
            "nothing to derive a palette from"
        )

    # One row, one pixel per sample: no padding, so no synthetic colour can leak
    # into the palette the way padding with black would.
    strip = Image.new("RGB", (len(pixels), 1))
    strip.putdata(pixels)
    quantised = strip.quantize(colors=colors, method=Image.MEDIANCUT,
                               dither=Image.Dither.NONE)
    flat = quantised.getpalette()[: colors * 3]
    palette = sorted({tuple(flat[i:i + 3]) for i in range(0, len(flat), 3)})

    # Sorted for a stable file: re-deriving from the same art must produce the
    # same bytes, so a diff means the art changed rather than the run.
    palette = sorted(palette, key=lambda c: (c[0] + c[1] + c[2], c))

    root = save_palette(out, palette, sprites_dir, len(paths),
                        f"median-cut over opaque pixels, {colors} colours")
    # How much of the source art the palette actually represents. This is the
    # number that decides whether the size is right, and it is not intuitive:
    # median-cut picks *representative* colours, so a palette can look plausible
    # and contain almost none of the art's exact colours. Measured on
    # data/sprites -- 17,050 distinct colours over 34,756 opaque pixels -- a
    # 32-colour palette covers 0% of them, 128 covers 17%, 256 covers 24%. So
    # snapping to a palette this size is a full recolour of the art, not a tidy-up.
    lookup = set(palette)
    covered = sum(1 for p in pixels if p in lookup)
    distinct = len(set(pixels))
    pct = 100.0 * covered / len(pixels)
    print(f"derived {len(palette)} colours from {len(paths)} sprites "
          f"({len(pixels)} opaque pixels, {distinct} distinct)")
    print(f"  coverage of the source art: {pct:.1f}% "
          f"(the rest would be recoloured by `lock`)")
    print(f"wrote {root}.json and {root}.png")

    if distinct > 4 * colors:
        print(f"\nNOTE: the source art has {distinct} distinct colours for a "
              f"{colors}-colour palette, so `lock` will recolour nearly all of it.")
        print("      That is correct for snapping GENERATED art to a pack's look.")
        print("      It is destructive if aimed at the pack itself: raise --colors,")
        print("      or point --sprites at the generated set and leave the pack alone.")
    return 0


# --------------------------------------------------------------------------
# lock / report
# --------------------------------------------------------------------------


def measure(sprite: Image.Image, palette) -> tuple[int, int, int]:
    """(opaque pixels outside the palette, semi-transparent pixels, opaque pixels).

    The denominator is deliberately the OPAQUE count, not the pixel count.
    Dividing by every pixel of a 32x32 cell understates the recolour share by
    whatever fraction the sprite does not fill, which for a small prop is most of
    it -- this metric read 59% for a palette that in fact contains almost none of
    the art's colours, and the error was the denominator.
    """
    outside = 0
    translucent = 0
    opaque = 0
    lookup = set(palette)
    for r, g, b, a in sprite.getdata():
        if 0 < a < 255:
            translucent += 1
        if a < DEFAULT_ALPHA_THRESHOLD:
            continue
        opaque += 1
        if (r, g, b) not in lookup:
            outside += 1
    return outside, translucent, opaque


def process(sprites_dir: str, palette, out_dir: str | None,
            alpha_threshold: int) -> tuple[int, int]:
    """Snap every sprite. Writes only when out_dir is given."""
    paths = sprite_paths(sprites_dir)
    palette_lab = [rgb_to_oklab(c) for c in palette]
    cache: dict[tuple[int, int, int], int] = {}

    total_outside = 0
    total_opaque = 0
    total_translucent_before = 0
    total_translucent_after = 0
    changed = 0

    if out_dir is not None:
        os.makedirs(out_dir, exist_ok=True)

    for path in paths:
        sprite = Image.open(path).convert("RGBA")
        before = list(sprite.getdata())
        outside, translucent, opaque = measure(sprite, palette)
        total_outside += outside
        total_opaque += opaque
        total_translucent_before += translucent

        if out_dir is None:
            total_translucent_after += translucent
            continue

        out_pixels = []
        for r, g, b, a in before:
            # Alpha first: a rim pixel is either in or out, never half, so the
            # anti-aliased edge against the chroma-key background is deleted
            # rather than merely recoloured.
            if alpha_threshold > 0:
                a = 255 if a >= alpha_threshold else 0
            if a == 0:
                out_pixels.append((0, 0, 0, 0))
                continue
            pr, pg, pb = palette[nearest_index((r, g, b), palette, palette_lab, cache)]
            if (pr, pg, pb) != (r, g, b):
                changed += 1
            if 0 < a < 255:
                total_translucent_after += 1
            out_pixels.append((pr, pg, pb, a))

        result = Image.new("RGBA", sprite.size)
        result.putdata(out_pixels)
        result.save(os.path.join(out_dir, os.path.basename(path)))

    pct = (100.0 * total_outside / total_opaque) if total_opaque else 0.0
    print(f"{len(paths)} sprites, {total_opaque} opaque pixels")
    print(f"  recolour share      : {pct:.1f}% "
          f"({total_outside} opaque pixels are not in the palette)")
    print(f"  semi-transparent    : {total_translucent_before} before, "
          f"{total_translucent_after} after")
    if out_dir is not None:
        print(f"  pixels recoloured   : {changed}")
        print(f"  wrote               : {out_dir}")
    return total_outside, total_opaque


# --------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Derive a shared sprite palette, and snap sprites to it.")
    sub = parser.add_subparsers(dest="command", required=True)

    p_derive = sub.add_parser("derive", help="build a palette from sprites")
    p_derive.add_argument("--sprites", required=True,
                          help="directory of sprites to learn the palette from")
    p_derive.add_argument("--colors", type=int, default=64,
                          help="palette size (default 64). Size it to the pack, "
                               "not to pixel-art orthodoxy: `derive` prints how "
                               "much of the art the palette covers, and a smooth-"
                               "shaded pack needs 128-256 to stay recognisable")
    p_derive.add_argument("--out", default="",
                          help="output stem, e.g. data/palette -> "
                               "data/palette.json + data/palette.png. "
                               "Default: <sprites>/../palette")

    p_lock = sub.add_parser("lock", help="snap sprites to a palette")
    p_lock.add_argument("--palette", required=True,
                        help="palette.json from `derive`")
    p_lock.add_argument("--sprites", required=True, help="directory to read")
    p_lock.add_argument("--out", required=True,
                        help="directory to write snapped sprites to. Must differ "
                             "from --sprites: this never edits art in place")
    p_lock.add_argument("--alpha-threshold", type=int,
                        default=DEFAULT_ALPHA_THRESHOLD,
                        help="alpha at or above this becomes opaque, below it "
                             "transparent. 0 keeps the original alphas "
                             f"(default {DEFAULT_ALPHA_THRESHOLD})")

    p_report = sub.add_parser("report", help="measure without writing")
    p_report.add_argument("--palette", required=True)
    p_report.add_argument("--sprites", required=True)

    args = parser.parse_args()

    if args.command == "derive":
        out = args.out or os.path.join(os.path.dirname(os.path.abspath(args.sprites)),
                                       "palette")
        return derive(args.sprites, args.colors, out)

    palette = load_palette(args.palette)
    palette_lab = [rgb_to_oklab(c) for c in palette]

    if args.command == "report":
        print(f"palette: {len(palette)} colours from {args.palette}")
        process(args.sprites, palette, None, 0)
        return 0

    # lock
    if os.path.abspath(args.out) == os.path.abspath(args.sprites):
        raise SystemExit(
            "palette.py: --out must differ from --sprites. Snapping is lossy and "
            "the whole point of the generated layer is that the art pack is never "
            "edited in place -- see docs/asset-generation-design.md 1.2."
        )
    print(f"palette: {len(palette)} colours from {args.palette}")
    process(args.sprites, palette, args.out, args.alpha_threshold)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
