"""Cut a Gemini sprite sheet into game-ready 32x32 RGBA sprites.

Why it works this way:
  * The sheet is JPEG, so the magenta background is not one exact colour -- it
    varies by +/-10 per channel and bleeds around the white grid lines. A global
    "replace this colour" would miss pixels; a wide fuzz would eat real magenta
    inside a sprite (stained glass, the red carpet). So the background is found by
    flood fill from each cell's own edge: only background *connected to the
    outside* is removed, which cannot touch interior detail.
  * Sprites are cropped to their content before downscaling, so a tall thin sprite
    like the cross keeps its pixels instead of being shrunk to fit empty padding.
  * Resizing is always NEAREST. Any smoothing filter turns pixel art into blur.
  * Each sprite is centred on a transparent 32x32 canvas, which is the shape the
    engine's DrawSpriteFitted expects.
"""
import os
import sys
from collections import deque
from PIL import Image

COLS, ROWS = 6, 4
# Measured from the sheet: ~10px inner margin plus a thin white separator.
MARGIN_X, MARGIN_Y = 12, 12
CELL_W = (2741 - 2 * MARGIN_X) / COLS
CELL_H = (1530 - 2 * MARGIN_Y) / ROWS
TARGET = 32
BG_TOL = 70          # generous: JPEG noise needs it, and flood fill keeps it safe
PAD = 3              # pixels of breathing room when cropping to content

# Names by (row, col), 0-based. Cell (0,0) is the church building, which is a
# multi-tile object and cannot be expressed in the current engine -- skipped.
NAMES = {
    (0, 1): "church_door",
    (0, 2): "rose_window",
    (0, 3): "stained_glass_window",
    (0, 4): "bell_tower_top",
    (0, 5): "stone_archway",
    (1, 0): "church_pew",
    (1, 1): "church_bench",
    (1, 2): "pulpit",
    (1, 3): "altar",
    (1, 4): "lectern",
    (1, 5): "collection_box",
    (2, 0): "chalice",
    (2, 1): "cross",
    (2, 2): "candle",
    (2, 3): "candelabra",
    (2, 4): "baptismal_font",
    (2, 5): "censer",
    (3, 0): "stained_glass_fragment",
    (3, 1): "stone_floor_tile",
    (3, 2): "red_carpet",
    (3, 3): "mossy_stone_wall",
    (3, 4): "wall_banner",
    (3, 5): "potted_plant",
}

def is_bg(px, tol=BG_TOL):
    r, g, b = px[:3]
    return r > 150 and b > 150 and g < 120 and abs(r - b) < 100

def cut_cell(img, col, row, name, outdir):
    x0 = int(round(MARGIN_X + col * CELL_W))
    y0 = int(round(MARGIN_Y + row * CELL_H))
    x1 = int(round(MARGIN_X + (col + 1) * CELL_W))
    y1 = int(round(MARGIN_Y + (row + 1) * CELL_H))
    cell = img.crop((x0, y0, x1, y1)).convert("RGB")
    w, h = cell.size
    px = cell.load()

    # Flood fill background from every border pixel, 4-connected.
    bg = bytearray(w * h)
    q = deque()
    for x in range(w):
        for y in (0, h - 1):
            if is_bg(px[x, y]) and not bg[y * w + x]:
                bg[y * w + x] = 1; q.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            if is_bg(px[x, y]) and not bg[y * w + x]:
                bg[y * w + x] = 1; q.append((x, y))
    while q:
        x, y = q.popleft()
        for nx, ny in ((x+1,y),(x-1,y),(x,y+1),(x,y-1)):
            if 0 <= nx < w and 0 <= ny < h:
                i = ny * w + nx
                if not bg[i] and is_bg(px[nx, ny]):
                    bg[i] = 1; q.append((nx, ny))

    # Content bounding box, then crop with padding.
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            if not bg[y * w + x]:
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    if maxx < 0:
        return False, 0, "empty cell"
    minx = max(0, minx - PAD); miny = max(0, miny - PAD)
    maxx = min(w - 1, maxx + PAD); maxy = min(h - 1, maxy + PAD)

    # Build RGBA from the crop.
    cw, ch = maxx - minx + 1, maxy - miny + 1
    sprite = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    sp = sprite.load()
    for y in range(ch):
        for x in range(cw):
            sx, sy = minx + x, miny + y
            if not bg[sy * w + sx]:
                sp[x, y] = px[sx, sy] + (255,)

    # Scale to fit 32x32, preserving aspect, then centre on a 32x32 canvas.
    scale = min(TARGET / cw, TARGET / ch)
    nw = max(1, min(TARGET, int(round(cw * scale))))
    nh = max(1, min(TARGET, int(round(ch * scale))))
    resized = sprite.resize((nw, nh), Image.NEAREST)

    canvas = Image.new("RGBA", (TARGET, TARGET), (0, 0, 0, 0))
    canvas.paste(resized, ((TARGET - nw) // 2, (TARGET - nh) // 2))
    path = os.path.join(outdir, name + ".png")
    canvas.save(path)
    return True, cw * ch, f"{cw}x{ch} -> {nw}x{nh}"

def main():
    sheet = sys.argv[1]
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    img = Image.open(sheet)
    print(f"sheet {img.size[0]}x{img.size[1]} -> {COLS}x{ROWS} grid, "
          f"cell {CELL_W:.0f}x{CELL_H:.0f}")
    ok = 0
    for (row, col), name in sorted(NAMES.items()):
        good, _area, info = cut_cell(img, col, row, name, outdir)
        print(f"  {'OK ' if good else 'SKIP'} r{row}c{col} {name:<24} {info}")
        ok += good
    print(f"\n{ok}/{len(NAMES)} sprites written to {outdir}")

if __name__ == "__main__":
    main()
