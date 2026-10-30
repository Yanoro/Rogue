#!/usr/bin/env python3
"""Slice the single object sheet into 32x32 sprites, keyed and despilled.

Scratch tooling; not part of the game build. Shares its matte approach with
make_sheet.py (which did the agents) so the two sets are processed identically:

 * KEY by direct colour match, not a corner flood fill. A flood fill chains
   through the anti-aliased rim and eats into the art once the fuzz is high
   enough to clear the rim (measured on the agents: 15% leaves pockets, 20%
   reaches skin). A direct match cannot chain.
 * DESPILL only along the silhouette, because the same rule applied everywhere
   erased real artwork (the trader's tunic) and left holes through solid cloth.

The one thing this sheet needs that the agent sheet did not is that positional
order is NOT trusted. The model drew 20 cells instead of the 19 asked for,
duplicating iron_vein in place of apple_tree and pushing everything after it down
one slot. So the mapping from cell to template key is explicit and is verified by
eye against a labelled contact sheet, rather than assumed from the prompt order.

Usage:  nix-shell -p imagemagick --run "python3 sprite_preview/slice_object_sheet.py"

Or point it at a different sheet without editing this file:
  python3 sprite_preview/slice_object_sheet.py --sheet raw/other_sheet.png \\
      --out other_out --number
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RAW = os.path.join(HERE, "raw", "objects_sheet.png")
WORK = os.path.join(HERE, "work")
OUT = os.path.join(HERE, "objects_out")
SHEET = os.path.join(HERE, "object_sprites_sheet.png")

# The pack's shared palette, if one has been derived:
#   python3 tools/palette.py derive --sprites data/sprites --colors 128
# When it exists every sprite is remapped to it instead of being quantised to a
# palette of its own, which is what made each generated sprite a different set of
# colours from its neighbours. See docs/asset-generation-design.md 7.3.
# ImageMagick's -remap takes a reference *image*, which is what palette.png is.
PALETTE = os.path.join(ROOT, "data", "palette.png")


def palette_args():
    """The ImageMagick colour step, and a label for the log."""
    if os.path.exists(PALETTE):
        return ["-remap", PALETTE], os.path.basename(PALETTE)
    return ["-colors", "16"], "-colors 16 (no data/palette.png)"

LABEL_FONT = os.path.join(
    ROOT, "fonts/oldschool/ttf - Px (pixel outline)/PxPlus_IBM_CGA.ttf"
)
BACKDROP = "#1b1b1f"
SHADOW = "#3a3a3a"

TILE = 32
FIT = 30
ZOOM = 6
FUZZ = 20

# Detection order, read off the numbered sheet (object_blob_indices.png), NOT off
# the order the prompt asked for. The two disagree: the model drew 20 cells
# instead of 19, duplicating iron_vein where apple_tree was requested, and it put
# the chest before the oven and the smelter rather than last. The first attempt at
# this table trusted the prompt order and mislabelled three sprites, which is
# exactly what the numbered sheet exists to catch.
READING_ORDER = [
    "apple",                # 0
    "bread",                # 1
    "bronze_coins",         # 2
    "iron_ingot",           # 3
    "flour",                # 4
    "iron_ore",             # 5
    "iron_scythe",          # 6
    "wheat",                # 7
    "wheat_seed",           # 8
    "wheat_seed_planted",   # 9
    "wheat_sprout",         # 10
    "wheat_mature",         # 11
    None,                   # 12 duplicate iron_vein, drawn in apple_tree's slot
    "iron_vein",            # 13
    "anvil",                # 14
    "apple_tree",           # 15
    "mill",                 # 16 the windmill
    "chest",                # 17
    "oven",                 # 18
    "smeltery",             # 19
]


def magick(*args):
    result = subprocess.run(["magick", *args], capture_output=True, text=True,
                            check=False)
    if result.returncode != 0:
        sys.exit(f"magick failed: {' '.join(args)}\n{result.stderr.strip()}")
    return result.stdout


def key_background(source, destination):
    background = magick(source, "-format", "%[pixel:p{3,3}]", "info:").strip()
    magick(source, "-alpha", "set", "-fuzz", f"{FUZZ}%", "-fill", "none",
           "-opaque", background, destination)
    return background


def remove_magenta_cast(source, destination, radius=2):
    """Clear magenta-cast pixels along the silhouette only (see make_sheet.py)."""
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


def find_blobs(keyed, min_area=300):
    """Every opaque blob's bounding box, in reading order.

    min_area is defaulted to 300 rather than hardcoded because that value is tuned
    to the canvas this script was written against -- and note the tile slicer's
    equivalent is 900, which is the same lesson in one line: a threshold is a
    property of the sheet's size, not of the pipeline, so a sheet generated at a
    different resolution needs it retuned or blobs vanish silently.
    """
    listing = magick(
        keyed,
        "-alpha", "extract", "-threshold", "40%",
        "-define", "connected-components:verbose=true",
        "-define", f"connected-components:area-threshold={min_area}",
        "-connected-components", "8", "null:",
    )
    boxes = []
    for line in listing.splitlines():
        match = re.search(
            r"(\d+)x(\d+)\+(\d+)\+(\d+)\s+([\d.]+),([\d.]+)\s+\d+\s+"
            r"srgb\((\d+),(\d+),(\d+)\)",
            line,
        )
        if not match:
            continue
        width, height, x, y, cx, cy, red, green, blue = match.groups()
        if int(red) + int(green) + int(blue) < 128:  # the keyed-out background
            continue
        boxes.append(
            {
                "x": int(x), "y": int(y),
                "w": int(width), "h": int(height),
                "cx": float(cx), "cy": float(cy),
            }
        )

    # Reading order: group into rows by vertical overlap, then sort each row by x.
    # A plain (y, x) sort would mis-order objects that sit slightly high or low in
    # the same visual row, which this sheet's hand-placed cells do.
    boxes.sort(key=lambda b: b["cy"])
    rows = []
    for box in boxes:
        for row in rows:
            if abs(row[0]["cy"] - box["cy"]) < max(row[0]["h"], box["h"]) * 0.6:
                row.append(box)
                break
        else:
            rows.append([box])
    ordered = []
    for row in rows:
        ordered.extend(sorted(row, key=lambda b: b["cx"]))
    return ordered


def coverage(path):
    listing = magick(path, "-depth", "8", "txt:-")
    opaque = total = 0
    for line in listing.splitlines()[1:]:
        match = re.search(r"\((\d+),(\d+),(\d+),(\d+)\)", line)
        if not match:
            continue
        total += 1
        if int(match.group(4)) > 0:
            opaque += 1
    return opaque / total if total else 0.0


def parse_args():
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sheet", default=RAW, help="the sheet to slice")
    parser.add_argument("--out", default=OUT, help="where the 32px sprites go")
    parser.add_argument("--work", default=WORK, help="scratch directory")
    parser.add_argument("--number", action="store_true",
                        help="write a labelled blob index sheet and stop. The "
                             "mapping is read off that sheet, never assumed")
    parser.add_argument("--expect", type=int, default=len(READING_ORDER),
                        help="how many subjects the sheet should hold "
                             f"(default {len(READING_ORDER)}, from READING_ORDER)")
    parser.add_argument("--min-blob", type=int, default=300,
                        help="connected-component area threshold in pixels "
                             "(default 300, tuned to the original canvas size)")
    return parser.parse_args()


def main():
    args = parse_args()
    global RAW, OUT, WORK, SHEET
    RAW = args.sheet
    OUT = args.out
    WORK = args.work
    stem = os.path.splitext(os.path.basename(RAW))[0]
    SHEET = os.path.join(HERE, f"{stem}_sprites_sheet.png")

    if not os.path.exists(RAW):
        sys.exit(f"no sheet at {RAW}")

    os.makedirs(WORK, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)

    _, colour_label = palette_args()
    print(f"colour step: {colour_label}")
    print(f"sheet      : {os.path.relpath(RAW, ROOT)}")

    keyed = os.path.join(WORK, "objects_keyed.png")
    background = key_background(RAW, keyed)
    print(f"backdrop keyed: {background}")

    blobs = find_blobs(keyed, args.min_blob)
    print(f"found {len(blobs)} blobs (area threshold {args.min_blob}px), "
          f"expecting {args.expect}")

    # Fewer blobs than subjects is the dangerous direction: a subject is missing.
    # More is normal here -- a held prop detaches into its own blob, and a model
    # that fills an "empty" cell with flat background produces another -- which is
    # why the clustering below exists. So only the shortfall is a warning.
    if len(blobs) < args.expect:
        print(f"WARNING: asked for {args.expect} subjects, found {len(blobs)} blobs "
              f"({len(blobs) - args.expect:+d}) -- a subject is likely missing, or a "
              f"blob fell below the")
        print(f"         area threshold (try --min-blob). Check the labelled sheet "
              f"before trusting any name.")
    elif len(blobs) > args.expect:
        print(f"note: {len(blobs) - args.expect} more blob(s) than subjects -- normal "
              f"for a detached prop or a")
        print(f"      background-filled empty cell. The labelled sheet says which.")

    if args.number:
        destination = os.path.join(HERE, f"{stem}_blob_indices.png")
        build_index_sheet(blobs, destination)
        print(f"wrote {destination}")
        for index, box in enumerate(blobs):
            print(f"  [{index:2}] box={box['w']}x{box['h']} "
                  f"at {box['x']},{box['y']} cy={box['cy']:.0f}")
        return

    written = []
    for index, box in enumerate(blobs):
        key = READING_ORDER[index] if index < len(READING_ORDER) else None
        if key is None:
            print(f"  [{index:2}] SKIPPED (duplicate) box={box['w']}x{box['h']}")
            continue

        trim = os.path.join(WORK, f"obj_{key}_trim.png")
        magick(keyed, "-crop",
               f"{box['w']}x{box['h']}+{box['x']}+{box['y']}", "+repage", trim)
        clean = os.path.join(WORK, f"obj_{key}_clean.png")
        remove_magenta_cast(trim, clean)
        clean = apply_fixups(clean, key)

        colour, _label = palette_args()
        magick(clean, "-background", "none", "-filter", "Lanczos",
               "-resize", f"{FIT}x{FIT}", "-dither", "None", *colour,
               "-gravity", "center", "-extent", f"{TILE}x{TILE}",
               os.path.join(OUT, f"{key}_32q.png"))
        # The point-sampled variant gets the same palette. The two differ in how
        # they resize -- smooth tone versus hard pixel edges -- which is a
        # separate question from which colours are allowed, and leaving one
        # unquantised would put hundreds-of-colour sprites in a 128-colour pack.
        magick(clean, "-background", "none", "-filter", "point",
               "-resize", f"{FIT}x{FIT}", "-dither", "None", *colour,
               "-gravity", "center", "-extent", f"{TILE}x{TILE}",
               os.path.join(OUT, f"{key}_32p.png"))
        written.append(key)
        print(f"  [{index:2}] {key:<20} box={box['w']}x{box['h']} "
              f"fill={coverage(os.path.join(OUT, f'{key}_32p.png')):.0%}")

    build_sheet(written)
    print(f"\nwrote {len(written)} sprites to {OUT}")
    print(f"wrote {SHEET}")


# Per-object touch-ups for content the model added that the prompt forbade, as
# explicit geometry rather than a silent edit so the fixup stays reproducible.
#
# The flour sack came back with the word FLOUR printed across it, despite the
# prompt's ban on text. Smoothing was tried first and failed: the letters are 4px
# strokes 28px tall, so every window wide enough to erase them also smeared the
# sack's shading, and a smaller one left the word legible -- median, mean and
# Gaussian variants were all compared side by side and none was clean. Erasing the
# lettering by *area* works instead, because the sack's front is a single flat
# colour there: sampling the band gives (231,206,164) for every surrounding pixel,
# so a fill of exactly that value is invisible. Geometry is in the
# full-resolution trim's coordinates, which is why it is applied before the
# downscale, and the rectangle was measured from an ASCII map of the dark pixels
# rather than estimated.
FIXUPS = {
    "flour": ("24,85 129,117", (231, 206, 164)),
}


def apply_fixups(source, key):
    fixup = FIXUPS.get(key)
    if fixup is None:
        return source
    rectangle, (red, green, blue) = fixup
    destination = os.path.join(WORK, f"obj_{key}_fixed.png")
    magick(source, "-fill", f"srgb({red},{green},{blue})",
           "-draw", f"rectangle {rectangle}", destination)
    print(f"       fixup: flat-filled {rectangle} to erase baked-in text")
    return destination


def build_index_sheet(blobs, destination):
    """Numbered contact sheet of the detected blobs, in detection order.

    This exists because the mapping from cell to template key was wrong the first
    time: the model's layout drifted (a duplicate, an extra row), so the order the
    prompt asked for is not the order on the sheet. Reading the mapping off this
    sheet and then slicing by index makes the assignment verifiable instead of
    assumed -- the numbers here are the very indices main() matches against.
    """
    per_row = 5
    zoom = ZOOM * TILE
    cell_w, cell_h = 160, zoom + 26
    rows_count = (len(blobs) + per_row - 1) // per_row

    header = os.path.join(WORK, "idx_header.png")
    magick("-size", f"{cell_w * per_row}x40", f"xc:{BACKDROP}",
           "-font", LABEL_FONT, "-fill", "#cccccc", "-pointsize", "15",
           "-annotate", "+10+26",
           "detected blobs in detection order -- read the mapping off this",
           header)

    rows = [header]
    for row_index in range(rows_count):
        cells = []
        for col in range(per_row):
            index = row_index * per_row + col
            cell = os.path.join(WORK, f"idx_{row_index}_{col}.png")
            if index >= len(blobs):
                magick("-size", f"{cell_w}x{cell_h}", f"xc:{BACKDROP}", cell)
            else:
                box = blobs[index]
                cut = os.path.join(WORK, f"idx_{index}_cut.png")
                magick(os.path.join(WORK, "objects_keyed.png"), "-crop",
                       f"{box['w']}x{box['h']}+{box['x']}+{box['y']}",
                       "+repage", cut)
                magick(
                    "-size", f"{cell_w}x{cell_h}", f"xc:{BACKDROP}",
                    "-fill", SHADOW, "-draw",
                    f"rectangle 10,0 {10 + zoom},{zoom}",
                    "(", cut, "-background", "none", "-filter", "point",
                    "-resize", f"{zoom}x{zoom}", ")",
                    "-gravity", "northwest", "-geometry", "+10+0", "-composite",
                    "-font", LABEL_FONT, "-fill", "#8fd0ff", "-pointsize", "16",
                    "-annotate", f"+10+{zoom + 18}", f"[{index}]",
                    cell,
                )
            cells.append(cell)
        row = os.path.join(WORK, f"idxrow_{row_index}.png")
        magick(*cells, "+append", row)
        rows.append(row)

    magick(*rows, "-append", destination)


def build_sheet(keys):
    """Labelled contact sheet, so every name can be checked against its image.

    Each cell is written to its own file and the rows are concatenated, rather
    than chaining every composite into one magick invocation. That chain silently
    collapsed to a single 192x192 sprite here (the same trap make_sheet.py hit):
    once `-composite` runs, later operators apply to the composited result, so a
    `-resize` meant for the next sprite lands on the canvas instead. Parenthesised
    groups plus separate files keep each step unambiguous.
    """
    per_row = 5
    zoom = ZOOM * TILE
    cell_w, cell_h = 220, zoom + 26
    rows_count = (len(keys) + per_row - 1) // per_row

    header = os.path.join(WORK, "obj_header.png")
    magick("-size", f"{cell_w * per_row}x40", f"xc:{BACKDROP}",
           "-font", LABEL_FONT, "-fill", "#cccccc", "-pointsize", "15",
           "-annotate", "+10+26",
           f"AI object sprites at 32px, shown {ZOOM}x  --  check every label",
           header)

    rows = [header]
    for row_index in range(rows_count):
        cells = []
        for col in range(per_row):
            index = row_index * per_row + col
            cell = os.path.join(WORK, f"cell_{row_index}_{col}.png")
            if index >= len(keys):
                magick("-size", f"{cell_w}x{cell_h}", f"xc:{BACKDROP}", cell)
            else:
                key = keys[index]
                magick(
                    "-size", f"{cell_w}x{cell_h}", f"xc:{BACKDROP}",
                    "-fill", SHADOW, "-draw",
                    f"rectangle 10,0 {10 + zoom},{zoom}",
                    "(", os.path.join(OUT, f"{key}_32p.png"),
                    "-filter", "point", "-resize", f"{zoom}x{zoom}!", ")",
                    "-geometry", "+10+0", "-composite",
                    "-font", LABEL_FONT, "-fill", "#e8c07a", "-pointsize", "14",
                    "-annotate", f"+10+{zoom + 18}", key,
                    cell,
                )
            cells.append(cell)
        row = os.path.join(WORK, f"objrow_{row_index}.png")
        magick(*cells, "+append", row)
        rows.append(row)

    magick(*rows, "-append", SHEET)


if __name__ == "__main__":
    main()
