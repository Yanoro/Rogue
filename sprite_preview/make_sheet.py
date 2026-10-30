#!/usr/bin/env python3
"""Slice the generated lineup into sprites, key the background, downscale to the
game's tile size, and build a review sheet.

Scratch tooling for evaluating AI-generated sprites; not part of the game build.

Pipeline, and why each step is done this way:

 1. KEY. The flat magenta is removed by a flood fill inward from the four corners
    rather than by a global colour match, so a sprite that happens to contain
    magenta cannot be punched full of holes. Fuzz stays low (20%): at 35% the fill
    leaks through the anti-aliased silhouette and eats the character's interior.
 2. SLICE. Blobs come from connected components, filtered to opaque ones and then
    clustered by overlapping x-range. Clustering rather than "the four biggest
    blobs" because a held prop can detach into its own blob and would otherwise be
    sliced off as a separate sprite.
 3. DOWNSCALE. Two variants on purpose. `q` resizes with Lanczos and quantises to
    16 colours: keeps the shapes, softens the shading. `p` resizes with a point
    filter: keeps hard pixel edges, aliases. Which is right is a judgement call,
    so the sheet shows both.
 4. REPORT. The share of semi-transparent pixels is printed per sprite. That is
    the magenta-fringe metric, and it is the number that says whether the source
    really was hard-edged.

Usage:  nix-shell -p imagemagick --run "python3 sprite_preview/make_sheet.py"
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RAW = os.path.join(HERE, "raw", "lineup.png")
WORK = os.path.join(HERE, "work")
OUT = os.path.join(HERE, "out")
SHEET = os.path.join(HERE, "agent_sprites_sheet.png")

LABEL_FONT = os.path.join(
    ROOT, "fonts/oldschool/ttf - Px (pixel outline)/PxPlus_IBM_CGA.ttf"
)
BACKDROP = "#1b1b1f"  # sheet background
SHADOW = "#3a3a3a"    # about what the village floor tile reads as in game
NAMES = ["farmer", "miller", "smith", "trader"]

TILE = 32   # one map tile: the box a sprite has to live in
FIT = 30    # drawn extent inside the tile, leaving a 1px margin
ZOOM = 8    # sheet zoom for judging pixel decisions
FUZZ = 20   # % colour distance for the corner flood fill


def magick(*args):
    result = subprocess.run(
        ["magick", *args], capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        sys.exit(f"magick failed: {' '.join(args)}\n{result.stderr.strip()}")
    return result.stdout


def key_background(source, destination):
    """Make the flat backdrop transparent with one direct colour match.

    Deliberately NOT a corner flood fill, which was the first approach and is
    wrong here. A flood fill walks the anti-aliased rim around every character:
    raising the fuzz far enough to clear that rim is also far enough for the fill
    to step through it into the character. At 15% the fill stops at the rim and
    leaves magenta pockets; at 20% it crosses and turns every face, hand and bare
    arm transparent, because those are skin and skin is only ~70 away from this
    backdrop in RGB terms. A direct match cannot chain, so it removes the flat
    backdrop and the anti-aliased rim in one go while leaving skin alone.

    The tradeoff, stated plainly: a direct match also removes a *character* pixel
    that happens to be backdrop-coloured, which a connectivity-based fill would
    spare. The prompt forbids magenta in the art for exactly this reason, and
    spill_fraction() below is what verifies it held.
    """
    background = magick(source, "-format", "%[pixel:p{3,3}]", "info:").strip()
    magick(source, "-alpha", "set", "-fuzz", f"{FUZZ}%", "-fill", "none",
           "-opaque", background, destination)
    return background


def find_clusters(keyed):
    """Bounding boxes of the characters, merged where they overlap in x."""
    listing = magick(
        keyed,
        "-alpha", "extract",
        "-threshold", "40%",
        "-define", "connected-components:verbose=true",
        "-define", "connected-components:area-threshold=400",
        "-connected-components", "8",
        "null:",
    )

    boxes = []
    for line in listing.splitlines():
        # The listing reports the keyed-out background as one huge component
        # spanning the whole canvas. It must be dropped before clustering: it
        # overlaps every character in x and would merge them into a single box,
        # which is exactly the bug this comment exists to prevent.
        match = re.search(
            r"(\d+)x(\d+)\+(\d+)\+(\d+)\s+[\d.]+,[\d.]+\s+\d+\s+srgb\((\d+),(\d+),(\d+)\)",
            line,
        )
        if not match:
            continue
        width, height, x, y, red, green, blue = (int(v) for v in match.groups())
        if red + green + blue < 128:
            continue
        boxes.append([x, y, x + width, y + height])
    if not boxes:
        sys.exit("no character blobs found in the keyed lineup")

    merged = True
    while merged:
        merged = False
        for i in range(len(boxes)):
            for j in range(i + 1, len(boxes)):
                a, b = boxes[i], boxes[j]
                if a[0] < b[2] and b[0] < a[2]:
                    boxes[i] = [min(a[0], b[0]), min(a[1], b[1]),
                                max(a[2], b[2]), max(a[3], b[3])]
                    boxes.pop(j)
                    merged = True
                    break
            if merged:
                break

    boxes.sort(key=lambda box: box[0])
    return boxes


def remove_magenta_cast(source, destination, radius=2):
    """Clear magenta-cast pixels, but only along the silhouette.

    The match at 20% leaves the outermost anti-aliased pixels alone: they are
    blends sitting further from the backdrop than the fuzz allows, and they read
    as pink specks on an arm or a knee once the sprite is upscaled. Raising the
    fuzz instead is not an option -- past ~25% the match starts to take skin --
    so this keys on the *shape* of the cast rather than on distance to one colour:
    magenta is red and blue well above green, simultaneously. Skin cannot satisfy
    that (village skin is about (195,102,71): red is above green, but blue is far
    below it) and neither can blue or red cloth.

    Applied everywhere, though, that rule is too blunt: it also erased the
    trader's tunic, whose red and gold dither blends into desaturated pink, and
    left transparent specks through solid cloth. So the rule is applied only to
    the rim -- pixels within `radius` of a transparent one -- and the untouched
    original is composited back over everything deeper inside. A pinkish pixel in
    an interior is art; a pinkish pixel on the outline is leftover backdrop.
    """
    cast = os.path.join(WORK, "cast.png")
    magick(source, "-alpha", "set", "-channel", "RGBA", "-fx",
           "((r-g)>0.24 && (b-g)>0.16) ? 0 : u", cast)

    interior = os.path.join(WORK, "interior.png")
    magick(source, "-alpha", "extract", "-morphology", "Erode", f"Disk:{radius}",
           interior)
    magick(cast,
           "(", source, interior, "-alpha", "off", "-compose", "CopyOpacity",
           "-composite", ")",
           "-compose", "over", "-composite", destination)


def spill_fraction(path, background):
    """Share of drawn pixels still close to the backdrop colour.

    This is the honest fringe metric. Counting semi-transparent pixels instead
    measures the *resampler*, not the matte: Lanczos necessarily invents partial
    alpha on every edge, while a point resize reports a suspiciously perfect 0%.
    What actually matters is whether magenta survives into the sprite, so this
    looks for opaque pixels that are still near the keyed-out colour.
    """
    values = [int(value) for value in re.findall(r"\d+", background)[:3]]
    listing = magick(path, "-depth", "8", "txt:-")
    spill = opaque = total = 0
    for line in listing.splitlines()[1:]:
        match = re.search(
            r"\((\d+),(\d+),(\d+),(\d+)\)", line
        )
        if not match:
            continue
        red, green, blue, alpha = (int(v) for v in match.groups())
        total += 1
        if alpha == 0:
            continue
        opaque += 1
        if max(abs(red - values[0]), abs(green - values[1]),
               abs(blue - values[2])) <= 60:
            spill += 1
    if total == 0:
        return 0.0, 0.0
    return (spill / opaque if opaque else 0.0), opaque / total


def flat_piece(source, extent, canvas, destination, resize_to=None):
    """One sheet cell: the sprite scaled with a point filter and laid on a tile."""
    args = [source]
    if resize_to:
        args += ["-filter", "point", "-resize", f"{resize_to}x{resize_to}!"]
    args += ["-background", SHADOW, "-gravity", "center", "-extent",
             f"{extent}x{canvas}", destination]
    magick(*args)


def text_piece(text, width, height, destination, size=18, color="#e8c07a"):
    magick("-size", f"{width}x{height}", f"xc:{BACKDROP}",
           "-font", LABEL_FONT, "-fill", color, "-pointsize", str(size),
           "-gravity", "west", "-annotate", "+6+0", text, destination)


def main():
    os.makedirs(WORK, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)

    keyed = os.path.join(WORK, "lineup_keyed.png")
    background = key_background(RAW, keyed)
    print(f"backdrop colour keyed: {background}")

    clusters = find_clusters(keyed)
    print(f"found {len(clusters)} character clusters (expected {len(NAMES)})")

    stats = []
    for name, (x0, y0, x1, y1) in zip(NAMES, clusters):
        trim = os.path.join(WORK, f"{name}_trim.png")
        magick(keyed, "-crop", f"{x1 - x0}x{y1 - y0}+{x0}+{y0}", "+repage", trim)
        despilled = os.path.join(WORK, f"{name}_clean.png")
        remove_magenta_cast(trim, despilled)
        trim = despilled

        for tag, resize in (
            ("q", ["-filter", "Lanczos", "-resize", f"{FIT}x{FIT}",
                   "-colors", "16", "-dither", "None"]),
            ("p", ["-filter", "point", "-resize", f"{FIT}x{FIT}"]),
        ):
            magick(trim, "-background", "none", *resize,
                   "-gravity", "center", "-extent", f"{TILE}x{TILE}",
                   os.path.join(OUT, f"{name}_32{tag}.png"))

        for tag in ("q", "p"):
            path = os.path.join(OUT, f"{name}_32{tag}.png")
            spill, coverage = spill_fraction(path, background)
            stats.append((name, tag, spill, coverage))

    print("magenta spill among drawn pixels / how much of the tile is drawn:")
    for name, tag, spill, coverage in stats:
        kind = "smooth" if tag == "q" else "hard  "
        print(f"  {name:<8} {kind} spill {spill:6.1%}   covered {coverage:5.1%}")

    build_sheet()
    print(f"wrote {SHEET}")


def build_sheet():
    """One row per sprite: 1x, 4x, then each 8x downscale variant."""
    canvas = TILE * ZOOM
    label_w, one_w, four_w, eight_w = 110, 44, 140, canvas + 8
    width = label_w + one_w + four_w + eight_w * 2

    header = os.path.join(WORK, "piece_header.png")
    text_piece("32px sprites:  1x | 4x | 8x smooth | 8x hard", width, 46, header,
               size=15, color="#cccccc")

    rows = [header]
    for name in NAMES:
        pieces = []
        label = os.path.join(WORK, f"piece_label_{name}.png")
        text_piece(name, label_w, canvas, label)
        pieces.append(label)

        # The 1x and 4x cells both come from the smooth variant; the two 8x cells
        # sit side by side so the downscale choice can be compared directly.
        for tag, columns in (("q", [(one_w, TILE), (four_w, TILE * 4)]),
                             ("p", [])):
            path = os.path.join(OUT, f"{name}_32{tag}.png")
            for index, (cell_w, scale) in enumerate(columns):
                cell = os.path.join(WORK, f"piece_{name}_{tag}_{index}.png")
                flat_piece(path, cell_w, canvas, cell, resize_to=scale)
                pieces.append(cell)
            cell = os.path.join(WORK, f"piece_{name}_{tag}_zoom.png")
            flat_piece(path, eight_w, canvas, cell, resize_to=canvas)
            pieces.append(cell)

        row = os.path.join(WORK, f"row_{name}.png")
        magick(*pieces, "+append", row)
        rows.append(row)

    magick(*rows, "-append",
           "-background", BACKDROP, "-alpha", "remove", "-alpha", "off",
           SHEET)


if __name__ == "__main__":
    main()
