#!/usr/bin/env python3
"""A stand-in for tools/gen_tile_art.py, for exercising the live queue for free.

It implements the same worklist contract -- read the JSON, write one sprite per
tile, write `<worklist>.result.json` -- but instead of calling an image API it emits
a flat 32x32 PNG in a colour taken from the derived palette. That makes `AIRogue
--tile-art <name> --tile-art-queue` a real test of the queue (request, batch window,
worker thread, main-thread drain, swap) with no network and no cost, which is the
only way the frame-safety half can be verified at all.

**It is not a substitute for the real generator.** It draws a solid colour, so
placement's palette contract is satisfied trivially. What it tests is the plumbing:
that a request is queued rather than run inline, that the frame loop keeps running
while the worker works, that the worker's result reaches the main thread, and that
the swap then attaches a sprite to the right cells. It needs nothing but the Python
standard library on purpose -- ImageMagick and Pillow are the *real* tool's problem,
and a stub that needed them could not run wherever the queue has to work.

Usage (as art.generation.command, with {worklist} substituted by the game):

  python3 tests/stub_tile_art.py {worklist}

Or by hand, on a worklist you write yourself:

  python3 tests/stub_tile_art.py /tmp/tile_art_worklist.json
"""

import json
import os
import struct
import sys
import zlib

SIZE = 32


def png_chunk(tag, payload):
    """One PNG chunk: length, tag, payload, CRC over tag+payload."""
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


def write_solid_png(path, rgb):
    """A 32x32 fully opaque PNG of one colour, with no dependencies.

    Hand-rolled rather than shelling out: the stub has to run wherever the queue
    runs, and the two image tools this project uses are both outside the default
    PATH here. A PNG this simple is a signature plus a zlib stream.
    """
    r, g, b = rgb[:3]
    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 2, 0, 0, 0)  # 8-bit truecolour
    raw = b"".join(b"\x00" + bytes((r, g, b)) * SIZE for _ in range(SIZE))
    data = (b"\x89PNG\r\n\x1a\n"
            + png_chunk(b"IHDR", header)
            + png_chunk(b"IDAT", zlib.compress(raw, 9))
            + png_chunk(b"IEND", b""))
    with open(path, "wb") as handle:
        handle.write(data)


def palette_colours(palette_json):
    try:
        with open(palette_json) as handle:
            colours = [tuple(c) for c in json.load(handle)["colors"]]
        if colours:
            return colours
    except (OSError, ValueError, KeyError):
        pass
    # Nothing derived: a mid grey. Placement's lock check will then refuse it, which
    # is the correct outcome and worth seeing rather than papering over.
    return [(128, 128, 128)]


def slugify(name):
    """The same rule as TileSpriteId, so the file lands where the engine looks."""
    slug = "tile_"
    pending = False
    for raw in name:
        if raw.isalnum():
            if pending and len(slug) > 5:
                slug += "_"
            pending = False
            slug += raw.lower()
        else:
            pending = True
    return slug


def write_result(worklist_path, ok, tiles, error="", cost=0.0):
    with open(worklist_path + ".result.json", "w") as handle:
        json.dump({"ok": ok, "tiles": tiles, "error": error, "cost": cost},
                  handle, indent=2)
        handle.write("\n")


def write_stage_files(worklist_path, tiles, colours):
    """Writes the same stage artifacts the real tool leaves behind.

    The pipeline panels read these by convention (a prompt, the sheet, and a
    `cell_<index>_{crop,trim,locked}.png` per cell), so a stub that wrote only the
    final sprites would leave the panels showing their empty state -- and the
    `--tile-art-hold` phase, whose whole purpose is to draw those panels against a
    settled job, could not tell "the panels drew" from "there was nothing to draw".
    These are flat colours too: this is about file layout, not pixels.
    """
    work_dir = os.path.dirname(os.path.abspath(worklist_path))
    with open(os.path.join(work_dir, "tile_art_sheet.prompt.txt"), "w") as handle:
        handle.write(
            "stub sheet prompt\n\n"
            + "\n".join(f"cell {i}: {name}" for i, name in enumerate(tiles))
            + "\n"
        )
    # One sheet holding every cell, then the per-cell stages, which is the layout
    # the panels look for.
    write_solid_png(os.path.join(work_dir, "tile_art_sheet.png"), colours[0])
    for index, _name in enumerate(tiles):
        colour = colours[index % len(colours)]
        for suffix in ("crop", "trim", "locked"):
            write_solid_png(
                os.path.join(work_dir, f"cell_{index}_{suffix}.png"), colour
            )


def main(argv):
    if len(argv) != 2:
        sys.exit("usage: stub_tile_art.py <worklist.json>")

    worklist_path = argv[1]
    try:
        with open(worklist_path) as handle:
            worklist = json.load(handle)
    except (OSError, ValueError) as error:
        write_result(worklist_path, False, [], f"unreadable worklist: {error}")
        return 1

    # The cells, with their kind and convention id. The `tiles` list alone is no longer
    # enough to name the output: an object's sprite is its bare key and a tile's is
    # `tile_<slug>`, so a stub that slugged every name the tile way would write a file
    # nothing looks for -- which is exactly the bug the real tool was fixed for. The
    # convention id is used verbatim when the worklist supplies it, and derived only as a
    # fallback for a hand-written worklist that omits it.
    cells = []
    for raw in worklist.get("cells") or []:
        if isinstance(raw, str):
            raw = {"name": raw}
        if not isinstance(raw, dict):
            continue
        name = str(raw.get("name") or "")
        if not name:
            continue
        kind = str(raw.get("kind") or "tile")
        convention = str(raw.get("conventionId") or "")
        if not convention:
            convention = slugify(name) if kind == "tile" else slugify(name)[
                len("tile_"):]
        cells.append({"name": name, "kind": kind, "conventionId": convention})
    if not cells:
        for raw in worklist.get("tiles") or []:
            name = str(raw)
            if name:
                cells.append({"name": name, "kind": "tile",
                              "conventionId": slugify(name)})
    tiles = [cell["name"] for cell in cells]

    out_dir = worklist.get("spritesDirectory") or ""
    if not tiles:
        write_result(worklist_path, False, [], "the worklist names no tiles")
        return 1
    if not out_dir:
        # The same refusal the real tool makes: with no generated layer the only
        # place left to write is the hand-authored art, which is never right.
        write_result(worklist_path, False, tiles,
                     "the worklist names no spritesDirectory")
        return 1

    os.makedirs(out_dir, exist_ok=True)
    colours = palette_colours(worklist.get("paletteJson") or "")
    for index, cell in enumerate(cells):
        colour = colours[index % len(colours)]
        write_solid_png(
            os.path.join(out_dir, cell["conventionId"] + ".png"), colour)
    write_stage_files(worklist_path, tiles, colours)

    write_result(worklist_path, True, tiles, "", 0.0)
    print(f"stub: wrote {len(tiles)} sprite(s) to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
