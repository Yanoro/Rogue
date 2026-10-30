#!/usr/bin/env python3
"""Slice the tile sheet into 32x32 map tiles, then prove they tile.

Scratch tooling; not part of the game build.

Tiles differ from objects in two ways that change this pipeline:

 * There is nothing to chroma-key *inside* a tile. A tile is a full-bleed
   texture, so the magenta is only the gutter between cells. Each cell is cropped
   to its blob and then INSET a few pixels, which is what removes both the
   anti-aliased rim where the texture met the background and any frame the model
   drew around the cell. A frame is fatal here in a way it never was for an
   object: it would repeat on every tile and read as a grid across the whole
   floor. The inset is in source pixels and costs well under one pixel at 32px.
 * Identification is by eye, not by position. The model filled 20 cells instead
   of the 11 asked for and repeated several textures to do it, so the order it
   was given is not the order on the sheet -- exactly the object-sheet failure.

The tiling proof is the point of the last step: every tile is printed as a 3x3
repeat, because a seam or a frame is invisible on a single tile and obvious on a
floor.

Usage:  nix-shell -p imagemagick --run "python3 sprite_preview/slice_tile_sheet.py [--number]"

Point it at a different sheet without editing this file:
  python3 sprite_preview/slice_tile_sheet.py --sheet raw/gap_tiles_sheet.png \\
      --out gap_tiles_out --number
  python3 sprite_preview/slice_tile_sheet.py --sheet raw/gap_tiles_sheet.png \\
      --out gap_tiles_out --expect 5 --map 0=anvil,1=forge,2=water_trough,3=workbench

--expect is the guard that matters when a sheet is generated: a model that drops a
cell leaves a perfectly valid sheet with one fewer blob, which nothing else here
notices. It warns rather than fails, because the area threshold below is a
property of the canvas size and a mismatch can be the threshold rather than the
model.
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RAW = os.path.join(HERE, "raw", "tiles_sheet.png")
WORK = os.path.join(HERE, "work")
OUT = os.path.join(HERE, "tiles_out")
INDEX_SHEET = os.path.join(HERE, "tile_blob_indices.png")
TILING_SHEET = os.path.join(HERE, "tile_tiling_check.png")

# The pack's shared palette, if one has been derived:
#   python3 tools/palette.py derive --sprites data/sprites --colors 128
# When it exists every tile is remapped to it instead of being quantised to a
# palette of its own. -remap takes a reference *image*, which is palette.png.
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

TILE = 32
ZOOM = 6
FUZZ = 20
# Source pixels cropped from every edge of a cell before the downscale. Removes
# the seam where the texture met the gutter and any drawn frame with it.
INSET = 8

# Per-tile inset, in source pixels, as (left, right, top, bottom).
#
# INSET above assumes the rim is the same width all the way round, which is all
# the regular tiles need. The floor's drawn rim is not even: it is roughly 5px on
# the left and top but 9px on the right and bottom, so a centred 8px cut still
# leaves a dark band along one side of every tile, and a tiled floor shows that
# band as a grid. Naming each edge takes the band off both sides. It stays a cut
# rather than the whole cell because the rim has to go -- keeping it is what
# makes one tile's edge meet the next one's.
INSET_OVERRIDES = {
    "floor": (5, 9, 5, 9),
}

# Blob index -> tile name, read off tile_blob_indices.png rather than off the
# order the prompt asked for. The model filled 17 cells instead of the 11 asked
# for and repeated textures to do it (tilled soil, water, stone wall, rock face
# and wooden floor each appear more than once), so most of this table is skips.
# `None` is a duplicate: the first good instance is kept and the rest dropped,
# which is the "filter" half of the batch.
MAPPING = {
    0: "floor",          # light grey flagstone
    1: "wooden_wall",    # vertical dark timber planks
    2: "grass",
    3: "fence",
    4: "tilled_soil",    # dark brown, parallel furrows
    5: "path",           # pale tan gravel
    6: "water",
    7: "stone_wall",     # grey blocks with mortar
    8: None,             # duplicate tilled soil
    9: "rock_face",
    10: None,            # duplicate water
    11: None,            # duplicate stone wall
    12: "packed_earth",
    13: None,            # duplicate rock face
    14: "wooden_floor",  # warm plank boards
    15: None,            # duplicate rock face
    16: None,            # duplicate wooden floor
}


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


def find_blobs(keyed, min_area=900):
    # min_area is a parameter because 900 is tuned to the canvas this script was
    # written against. A sheet generated at a different size puts the same object
    # at a different pixel area, so a hardcoded threshold silently drops real
    # blobs -- or keeps specks -- on any other sheet.
    listing = magick(
        keyed, "-alpha", "extract", "-threshold", "40%",
        "-define", "connected-components:verbose=true",
        "-define", f"connected-components:area-threshold={min_area}",
        "-connected-components", "8", "null:",
    )
    boxes = []
    for line in listing.splitlines():
        match = re.search(
            r"(\d+)x(\d+)\+(\d+)\+(\d+)\s+([\d.]+),([\d.]+)\s+\d+\s+"
            r"srgb\((\d+),(\d+),(\d+)\)", line)
        if not match:
            continue
        width, height, x, y, cx, cy, red, green, blue = match.groups()
        if int(red) + int(green) + int(blue) < 128:
            continue
        boxes.append({"x": int(x), "y": int(y), "w": int(width), "h": int(height),
                      "cx": float(cx), "cy": float(cy)})
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


def build_index_sheet(blobs, destination):
    per_row = 5
    zoom = 150
    cell_w, cell_h = zoom + 20, zoom + 30
    rows_count = (len(blobs) + per_row - 1) // per_row
    header = os.path.join(WORK, "tile_idx_header.png")
    magick("-size", f"{cell_w * per_row}x36", f"xc:{BACKDROP}",
           "-font", LABEL_FONT, "-fill", "#cccccc", "-pointsize", "14",
           "-annotate", "+8+24",
           "detected tile blobs in detection order -- read the mapping off this",
           header)
    rows = [header]
    for row_index in range(rows_count):
        cells = []
        for col in range(per_row):
            index = row_index * per_row + col
            cell = os.path.join(WORK, f"tileidx_{row_index}_{col}.png")
            if index >= len(blobs):
                magick("-size", f"{cell_w}x{cell_h}", f"xc:{BACKDROP}", cell)
            else:
                box = blobs[index]
                cut = os.path.join(WORK, f"tileidx_{index}_cut.png")
                magick(os.path.join(WORK, "tiles_keyed.png"), "-crop",
                       f"{box['w']}x{box['h']}+{box['x']}+{box['y']}",
                       "+repage", cut)
                magick("-size", f"{cell_w}x{cell_h}", f"xc:{BACKDROP}",
                       "(", cut, "-background", "none", "-filter", "point",
                       "-resize", f"{zoom}x{zoom}", ")",
                       "-gravity", "northwest", "-geometry", "+10+4", "-composite",
                       "-font", LABEL_FONT, "-fill", "#8fd0ff", "-pointsize", "18",
                       "-annotate", f"+10+{zoom + 24}", f"[{index}]", cell)
            cells.append(cell)
        row = os.path.join(WORK, f"tileidxrow_{row_index}.png")
        magick(*cells, "+append", row)
        rows.append(row)
    magick(*rows, "-append", destination)


def slice_tile(box, key):
    """Crop one cell, drop its rim, and downscale to a 32x32 tile."""
    trim = os.path.join(WORK, f"tile_{key}_full.png")
    magick(os.path.join(WORK, "tiles_keyed.png"), "-crop",
           f"{box['w']}x{box['h']}+{box['x']}+{box['y']}", "+repage", trim)

    # The rims are transparent after keying (the gutter and the anti-aliased edge
    # where the texture met it), so they can be discarded rather than guessed at.
    # INSET is applied to every edge; a per-tile override names each edge instead,
    # for a texture whose rim is not evenly wide (see INSET_OVERRIDES).
    if key in INSET_OVERRIDES:
        left, right, top, bottom = INSET_OVERRIDES[key]
    else:
        inset = min(INSET, box["w"] // 4, box["h"] // 4)
        left = right = top = bottom = inset

    inner = os.path.join(WORK, f"tile_{key}_inner.png")
    magick(trim, "-crop",
           f"{box['w'] - left - right}x{box['h'] - top - bottom}+{left}+{top}",
           "+repage", inner)

    # Lanczos then the pack's palette: a ground texture wants smooth tone, not
    # the hard point sampling the character sprites use, and it wants the same
    # colours as every other tile rather than a palette of its own.
    colour, _label = palette_args()
    magick(inner, "-filter", "Lanczos", "-resize", f"{TILE}x{TILE}!",
           "-dither", "None", *colour,
           "-alpha", "remove", "-alpha", "off",
           os.path.join(OUT, f"{key}.png"))
    return os.path.join(OUT, f"{key}.png")


def build_tiling_sheet(keys):
    """Every tile as a 3x3 repeat: a seam or a frame cannot hide here."""
    per_row = 4
    size = 96                      # 3 x 32
    zoom = 3
    cell = size * zoom + 30
    rows_count = (len(keys) + per_row - 1) // per_row
    header = os.path.join(WORK, "tiling_header.png")
    magick("-size", f"{cell * per_row}x40", f"xc:{BACKDROP}",
           "-font", LABEL_FONT, "-fill", "#cccccc", "-pointsize", "15",
           "-annotate", "+8+26",
           "each tile repeated 3x3 -- look for seams and frames, not the pattern",
           header)
    rows = [header]
    for row_index in range(rows_count):
        cells = []
        for col in range(per_row):
            index = row_index * per_row + col
            out = os.path.join(WORK, f"tile3x3_{row_index}_{col}.png")
            if index >= len(keys):
                magick("-size", f"{cell}x{cell}", f"xc:{BACKDROP}", out)
            else:
                key = keys[index]
                magick("-size", f"{size}x{size}",
                       f"tile:{os.path.join(OUT, key + '.png')}",
                       "-filter", "point", "-resize", f"{size * zoom}x{size * zoom}!",
                       "-background", BACKDROP, "-gravity", "north",
                       "-extent", f"{cell}x{cell}",
                       "-font", LABEL_FONT, "-fill", "#e8c07a", "-pointsize", "15",
                       "-annotate", f"+{cell // 2}+{size * zoom + 20}", key, out)
            cells.append(out)
        row = os.path.join(WORK, f"tilerow_{row_index}.png")
        magick(*cells, "+append", row)
        rows.append(row)
    magick(*rows, "-append", TILING_SHEET)


def parse_mapping(spec):
    """'0=anvil,1=forge' -> {0: 'anvil', 1: 'forge'}. Empty means 'use MAPPING'."""
    mapping = {}
    for item in spec.split(","):
        item = item.strip()
        if not item:
            continue
        if "=" not in item:
            sys.exit(f"--map: expected index=key, got '{item}'")
        index, key = item.split("=", 1)
        mapping[int(index)] = key.strip() or None
    return mapping


def parse_args():
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sheet", default=RAW, help="the sheet to slice")
    parser.add_argument("--out", default=OUT, help="where the 32px tiles are written")
    parser.add_argument("--work", default=WORK, help="scratch directory")
    parser.add_argument("--number", action="store_true",
                        help="write a labelled index sheet and stop. The mapping is "
                             "read off that sheet, never assumed from prompt order")
    parser.add_argument("--map", dest="mapping", default="",
                        help="blob index to tile key, e.g. 0=anvil,1=forge. "
                             "Overrides the MAPPING table below")
    parser.add_argument("--expect", type=int, default=0,
                        help="how many blobs the sheet should hold. Warns when the "
                             "count differs: a model that drops a cell leaves a "
                             "perfectly valid-looking sheet with one fewer blob")
    parser.add_argument("--min-blob", type=int, default=900,
                        help="connected-component area threshold in pixels "
                             "(default 900, tuned to the original canvas size)")
    return parser.parse_args()


def main():
    args = parse_args()

    # Rebound rather than threaded through every helper: the helpers already read
    # these as module globals, and adding a CLI is not worth rewriting them all.
    global RAW, OUT, WORK, INDEX_SHEET, TILING_SHEET, MAPPING
    RAW = args.sheet
    OUT = args.out
    WORK = args.work
    # Named after the input sheet, so slicing a second sheet does not overwrite the
    # first one's review artifacts.
    stem = os.path.splitext(os.path.basename(RAW))[0]
    INDEX_SHEET = os.path.join(HERE, f"{stem}_blob_indices.png")
    TILING_SHEET = os.path.join(HERE, f"{stem}_tiling_check.png")
    if args.mapping:
        MAPPING = parse_mapping(args.mapping)

    if not os.path.exists(RAW):
        sys.exit(f"no sheet at {RAW}")

    os.makedirs(WORK, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)

    _, colour_label = palette_args()
    print(f"colour step: {colour_label}")
    print(f"sheet      : {os.path.relpath(RAW, ROOT)}")

    keyed = os.path.join(WORK, "tiles_keyed.png")
    background = key_background(RAW, keyed)
    print(f"backdrop keyed: {background}")

    blobs = find_blobs(keyed, args.min_blob)
    smallest = min((max(b["w"], b["h"]) for b in blobs), default=0)
    largest = max((max(b["w"], b["h"]) for b in blobs), default=0)
    print(f"found {len(blobs)} tile blobs (area threshold {args.min_blob}px, "
          f"blob sizes {smallest}-{largest}px)")

    # The expectation is the number of SUBJECTS ASKED FOR, and it is only compared
    # when --expect says so. It deliberately does not default to len(MAPPING): a
    # mapping has one entry per subject it names, while a sheet can hold more blobs
    # than subjects, because a model that fills an "empty" cell with flat
    # background produces a full-cell blob and a stray swatch strip produces
    # another. Both were present on the first generated sheet here, so deriving the
    # expectation from the mapping warned about a sheet that was fine.
    #
    # Fewer blobs than subjects is the dangerous direction -- a subject is missing.
    # More is usually junk, so it is a note rather than a warning.
    if args.expect:
        if len(blobs) < args.expect:
            print(f"WARNING: asked for {args.expect} subjects, found {len(blobs)} "
                  f"blobs ({len(blobs) - args.expect:+d}) -- a subject is likely "
                  f"missing.")
            print("         Or a cell fell below the area threshold, which is common "
                  "on a sheet at")
            print("         a different size; try --min-blob. Read the index sheet "
                  "(--number) before")
            print("         trusting any name either way.")
        elif len(blobs) > args.expect:
            print(f"note: {len(blobs)} blobs for {args.expect} subjects -- the extra "
                  f"{len(blobs) - args.expect} are usually")
            print("      background-filled empty cells or stray marks. The index "
                  "sheet (--number) says which;")
            print("      they are excluded by simply not naming them in --map.")

    if args.number:
        build_index_sheet(blobs, INDEX_SHEET)
        print(f"wrote {INDEX_SHEET}")
        for index, box in enumerate(blobs):
            print(f"  [{index:2}] {box['w']}x{box['h']} at {box['x']},{box['y']}")
        return

    if not MAPPING:
        sys.exit("MAPPING is empty -- run with --number and read the sheet first")

    keys = []
    for index, key in sorted(MAPPING.items()):
        if index >= len(blobs):
            print(f"  [{index:2}] SKIPPED (no such blob)")
            continue
        if key is None or key in keys:
            print(f"  [{index:2}] SKIPPED (duplicate)")
            continue
        slice_tile(blobs[index], key)
        keys.append(key)
        print(f"  [{index:2}] {key}")

    build_tiling_sheet(keys)
    print(f"\nwrote {len(keys)} tiles to {OUT}")
    print(f"wrote {TILING_SHEET}")


if __name__ == "__main__":
    main()
