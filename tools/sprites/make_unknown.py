#!/usr/bin/env python3
"""Author the "Unknown" fallback sprite: what an entity draws when no sprite resolves.

Every tile, object and character now renders as either its sprite or this. It is
deliberately not subtle -- it is the visual form of "this thing has no art" -- but
it is also not a magenta debug texture: it uses the pack's dark slate and a warm
accent, so a map full of them still reads as a game rather than as a broken build.

    nix-shell -p python3Packages.pillow --run "python3 tools/sprites/make_unknown.py"

Writes data/sprites/unknown.png (32x32 RGBA, fully opaque: a tile has no
transparency behind it).
"""

import os

from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(REPO, "data", "sprites", "unknown.png")

SIZE = 32

PLATE = (27, 27, 36, 255)     # #1b1b24, the pack's near-black slate
BORDER = (58, 58, 76, 255)    # #3a3a4c
GLINT = (74, 74, 94, 255)     # top/left inner highlight
MARK = (224, 112, 60, 255)    # #e0703c, warm accent -- reads without shouting

# A 7x10 question mark, doubled to 14x20 and centred. Hand-placed rather than
# rendered from a font: the shipped bitmap font has no outline for stb_truetype to
# rasterize (which is the crash this change removes), and a placeholder that
# depends on a font is a placeholder that can fail to draw.
MARK_ROWS = [
    "..###..",
    ".#...#.",
    ".#...#.",
    ".....#.",
    "....#..",
    "...#...",
    "...#...",
    ".......",
    "...#...",
    "...#...",
]


def main():
    image = Image.new("RGBA", (SIZE, SIZE), PLATE)
    pixels = image.load()

    # Border + a one-pixel glint on the lit (top-left) edges, matching the pack's
    # light direction so the placeholder does not read as lifted from elsewhere.
    for i in range(SIZE):
        for x, y in ((i, 0), (i, SIZE - 1), (0, i), (SIZE - 1, i)):
            pixels[x, y] = BORDER
    for i in range(1, SIZE - 1):
        pixels[i, 1] = GLINT
        pixels[1, i] = GLINT

    scale = 2
    width = len(MARK_ROWS[0]) * scale
    height = len(MARK_ROWS) * scale
    origin_x = (SIZE - width) // 2
    origin_y = (SIZE - height) // 2
    for row, line in enumerate(MARK_ROWS):
        for col, c in enumerate(line):
            if c != "#":
                continue
            for dy in range(scale):
                for dx in range(scale):
                    pixels[origin_x + col * scale + dx,
                           origin_y + row * scale + dy] = MARK

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    image.save(OUT)
    print("wrote " + os.path.relpath(OUT, REPO))


if __name__ == "__main__":
    main()
