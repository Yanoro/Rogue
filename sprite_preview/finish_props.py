#!/usr/bin/env python3
"""Turn a 512px ComfyUI render into a finished 32x32 AIRogue prop sprite.

Scratch tooling; not part of the game build.

This is the missing half of the pipeline. `comfy_props.py` spends GPU time to
produce pixels; this decides which of those pixels survive into the game. Doing
it in a separate, re-runnable tool matters because it is free: a better
background-detection rule or a different downscale filter is worth retrying
twenty times, and re-rendering is not (`tools/README.md` §11.2, "keep the raw
renders, always").

The stages, and why each is where it is
---------------------------------------
1. **Find the background.** The render's background is not one colour. The first
   live barrel's "flat green" spanned thousands of near-identical values, so
   detection is a flood fill from the corners with a tolerance, not an equality
   test. Corners, not the whole border: a subject that touches one edge must not
   poison the estimate.
2. **Key it to transparent.** Real sprites in `data/sprites/` are RGBA. A prop
   that ships with a painted grey field will show that field on every map tile it
   is placed on.
3. **Crop to the subject, with a margin.** `apple.png` and `bread.png` fill most
   of their 32x32 frame. A render of the same object occupies ~27% of a 512px
   canvas, so the crop is what makes the subject the right *size* -- downscaling
   alone leaves a tiny prop adrift in its own sprite.
4. **Downscale.** LANCZOS by default, matching the soft-shaded look of the
   existing art pack (see compare_real.py: the real props are ~350 colours with
   smooth shading, not 16-colour hard pixel art). `--nearest` is available for a
   hard-edged style.
5. **Optional palette lock.** `--lock` quantises to `data/palette.json`, the 128
   colours median-cut from the real sprites. It is off by default because it is
   lossy and the real sprites are not palette-constrained; turn it on when a
   batch needs to be forced into one family.

Alpha handling is the subtle part: keying has to happen at 512px *before* the
downscale, or the background colour bleeds into the subject's edge pixels and
every sprite gets a halo. This is the same ordering rule as "quantise at 512
first, then NEAREST" from tools/README.md §5.

Usage:
    sprite_preview/run.sh finish_props.py                       # all of comfy_out
    sprite_preview/run.sh finish_props.py barrel_0 well_0 --lock
    sprite_preview/run.sh finish_props.py --tolerance 60 --zoom-crop
"""

import argparse
import json
import os
import sys
from collections import Counter, deque

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow missing; use sprite_preview/run.sh")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_DIR = os.path.join(HERE, "comfy_out")
FINAL_DIR = os.path.join(OUT_DIR, "final")
PALETTE_PATH = os.path.join(ROOT, "data", "palette.json")
GAME_DIR = os.path.join(ROOT, "data", "sprites")


# --------------------------------------------------------------------------
# Background detection and keying
# --------------------------------------------------------------------------

def near(a, b, tol):
    return (abs(a[0] - b[0]) <= tol and abs(a[1] - b[1]) <= tol
            and abs(a[2] - b[2]) <= tol)


def flood_background(img, tolerance=38):
    """Return a bytearray mask of the background, flooded from the four corners.

    A flood fill rather than "every pixel near the corner colour": subjects often
    contain a colour close to the background (grey stone on grey field), and
    keying by colour alone punches holes in the object. Filling from the corners
    keeps interior regions connected-to-nothing out of the mask.
    """
    w, h = img.size
    px = img.load()
    mask = bytearray(w * h)
    seeds = [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]
    # The background colour can differ per corner (a lighting gradient); each
    # corner seeds its own fill with its own colour.
    for sx, sy in seeds:
        if mask[sy * w + sx]:
            continue
        base = px[sx, sy]
        queue = deque([(sx, sy)])
        while queue:
            x, y = queue.popleft()
            index = y * w + x
            if mask[index]:
                continue
            if not near(px[x, y], base, tolerance):
                continue
            mask[index] = 1
            if x > 0:
                queue.append((x - 1, y))
            if x < w - 1:
                queue.append((x + 1, y))
            if y > 0:
                queue.append((x, y - 1))
            if y < h - 1:
                queue.append((x, y + 1))
    return mask


def background_coverage(mask):
    return 100.0 * sum(mask) / max(1, len(mask))


def subject_box(mask, w, h):
    """Bounding box of the non-background pixels, or None if nothing survived."""
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(h):
        row = y * w
        for x in range(w):
            if not mask[row + x]:
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                if y < miny:
                    miny = y
                if y > maxy:
                    maxy = y
    if maxx < 0:
        return None
    return (minx, miny, maxx, maxy)


def largest_component(mask, w, h, min_share=0.02):
    """Keep only the biggest connected non-background blob; return a new mask.

    This is what makes a soft contact shadow and a stray background speck
    survivable. The model routinely paints a low-contrast shadow or vignette
    around the subject, which the flood fill cannot key because it is not the
    background colour -- measured on one barrel, the extra region pushed the
    subject bounding box from 194x359 to the full 512x512. Cropping to that box
    would shrink the real subject to a quarter of its sprite.

    Discarding every blob except the largest removes both the shadow ring and any
    isolated speck *without* needing to guess a colour threshold, because the
    subject is by construction the largest connected region. `min_share` guards
    the pathological case where the largest blob is itself just noise: below that
    share of the frame, nothing is returned and the render is rejected.

    Also returns the share of the frame the surviving blob covers, so a caller
    can tell "clean object" from "object plus a lot of attached junk".
    """
    seen = bytearray(w * h)
    best = None
    best_size = 0
    for start in range(w * h):
        if mask[start] or seen[start]:
            continue
        component = []
        queue = deque([start])
        seen[start] = 1
        while queue:
            index = queue.popleft()
            component.append(index)
            x = index % w
            y = index // w
            if x > 0 and not mask[index - 1] and not seen[index - 1]:
                seen[index - 1] = 1
                queue.append(index - 1)
            if x < w - 1 and not mask[index + 1] and not seen[index + 1]:
                seen[index + 1] = 1
                queue.append(index + 1)
            if y > 0 and not mask[index - w] and not seen[index - w]:
                seen[index - w] = 1
                queue.append(index - w)
            if y < h - 1 and not mask[index + w] and not seen[index + w]:
                seen[index + w] = 1
                queue.append(index + w)
        if len(component) > best_size:
            best_size = len(component)
            best = component

    if best is None:
        return None, 0.0
    share = best_size / float(w * h)
    if share < min_share:
        return None, share

    # Rebuild the mask: background everywhere except the winning blob.
    out = bytearray(1 for _ in range(w * h))
    for index in best:
        out[index] = 0
    return out, share


def fill_holes(mask, w, h):
    """Mark background pixels not reachable from the border as subject.

    A subject with an enclosed region the background colour leaks into -- the
    space inside a cart's wheel spokes, a bucket's opening -- would otherwise be
    punched transparent. Re-scanning from the outside and inverting finds those
    enclosed regions exactly.
    """
    outside = bytearray(w * h)
    queue = deque()
    for x in range(w):
        for y in (0, h - 1):
            if mask[y * w + x] and not outside[y * w + x]:
                outside[y * w + x] = 1
                queue.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            if mask[y * w + x] and not outside[y * w + x]:
                outside[y * w + x] = 1
                queue.append((x, y))
    while queue:
        x, y = queue.popleft()
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= nx < w and 0 <= ny < h:
                index = ny * w + nx
                if mask[index] and not outside[index]:
                    outside[index] = 1
                    queue.append((nx, ny))
    # Anything still marked background but not reachable from outside is a hole.
    return bytearray(1 if (mask[i] and not outside[i]) else mask[i]
                     for i in range(w * h))


def key_out(img, mask, w, h, tolerance=38, hard=False, alpha_gain=1.0,
            erode=0, alpha_cut=0.0):
    """Build RGBA with background removed, and an anti-aliased edge.

    A binary mask is not enough. Where the subject meets the background the render
    has a band of pixels that are a *blend* of the two, and a hard cut leaves that
    band as a coloured fringe -- in the first finished barrels it showed up as a
    grey halo all the way around the silhouette, which is exactly the artefact
    that makes generated art look pasted on.

    `hard=True` cuts at the mask boundary. The default instead estimates what the
    background would have been at each blended pixel (by growing the background
    colour into the subject, one ring at a time) and solves for the alpha that
    would produce the observed colour:

        observed = alpha * subject + (1 - alpha) * background

    which is the standard compositing equation.

    Two knobs tame the case where the model painted a soft contact shadow under
    the subject, which is technically part of the subject but reads as dirt at
    32px:

      * `alpha_gain` -- how far from the background a pixel must be before it is
        fully opaque. Higher values are more willing to treat a faint difference
        as transparent, which eats a soft shadow.
      * `erode` -- shrink the opaque region by N pixels before the matte is
        solved. Removes a rim the flood fill missed entirely.
    """
    out = Image.new("RGBA", (w, h))
    src = img.load()
    dst = out.load()

    if erode > 0:
        # Shrink the *subject*: clear a boundary ring of subject pixels, repeat.
        shrunk = bytearray(mask)
        for _ in range(erode):
            nxt = bytearray(shrunk)
            for y in range(h):
                row = y * w
                for x in range(w):
                    index = row + x
                    if shrunk[index]:
                        continue
                    for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                        if 0 <= nx < w and 0 <= ny < h:
                            if shrunk[ny * w + nx]:
                                nxt[ny * w + nx] = 0
                                break
            shrunk = nxt
        mask = shrunk

    if hard:
        for y in range(h):
            row = y * w
            for x in range(w):
                r, g, b = src[x, y]
                dst[x, y] = (r, g, b, 0 if mask[row + x] else 255)
        return out

    # --- estimate the background colour behind every pixel ----------------
    # Seed the estimate from background pixels that are far from the subject, so
    # a gradient in the field is followed rather than flattened. A single global
    # colour would make the alpha wrong wherever the field is lighter or darker,
    # which is most renders (the first barrel's field ran from (169,167,172) at
    # the corner to noticeably darker at the bottom).
    est_r = [0] * (w * h)
    est_g = [0] * (w * h)
    est_b = [0] * (w * h)
    known = bytearray(w * h)
    queue = deque()
    for y in range(h):
        for x in range(w):
            index = y * w + x
            if mask[index]:
                r, g, b = src[x, y]
                est_r[index], est_g[index], est_b[index] = r, g, b
                known[index] = 1
                queue.append((x, y))
    # Also seed from the border so the fill has somewhere to start even if the
    # flood was too tight.
    for x in range(w):
        for y in (0, h - 1):
            index = y * w + x
            if not known[index] and mask[index]:
                r, g, b = src[x, y]
                est_r[index], est_g[index], est_b[index] = r, g, b
                known[index] = 1
                queue.append((x, y))

    # Grow the known background inward by repeatedly averaging neighbours.
    for _ in range(6):
        added = []
        for y in range(h):
            row = y * w
            for x in range(w):
                index = row + x
                if known[index]:
                    continue
                rs = gs = bs = 0
                n = 0
                for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                    if 0 <= nx < w and 0 <= ny < h:
                        ni = ny * w + nx
                        if known[ni]:
                            rs += est_r[ni]
                            gs += est_g[ni]
                            bs += est_b[ni]
                            n += 1
                if n:
                    added.append((x, y, rs // n, gs // n, bs // n))
        if not added:
            break
        for x, y, r, g, b in added:
            index = y * w + x
            est_r[index], est_g[index], est_b[index] = r, g, b
            known[index] = 1

    # --- solve for alpha --------------------------------------------------
    for y in range(h):
        row = y * w
        for x in range(w):
            index = row + x
            r, g, b = src[x, y]
            if mask[index]:
                # Pure background. Not fully invisible: keep a little alpha so a
                # later `--lock` or a deliberate shadow does not vanish, but 0 is
                # what the real props use, so match them.
                dst[x, y] = (r, g, b, 0)
                continue

            br, bg_, bb = est_r[index], est_g[index], est_b[index]
            # How far is this pixel from the estimated background?
            dist = (abs(r - br) + abs(g - bg_) + abs(b - bb)) / 3.0
            if dist <= 1.0:
                dst[x, y] = (r, g, b, 0)
                continue
            # Alpha from the largest channel displacement: a pixel half blended
            # toward a bright object is about half opaque. `alpha_gain` scales how
            # much displacement counts as fully opaque -- 96/channel at the
            # default, so raising it declares faint contact shadow transparent.
            deltas = []
            for obs, bkg in ((r, br), (g, bg_), (b, bb)):
                d = obs - bkg
                if abs(d) > 1.0:
                    deltas.append(min(1.0, abs(d) / 96.0))
            alpha = max(deltas) if deltas else 1.0
            alpha = min(1.0, max(0.0, alpha * alpha_gain))
            # Un-blend: recover the subject colour so the sprite does not carry a
            # background tint into the palette lock.
            # A faint blend at the silhouette edge is not part of the subject,
            # it is the anti-aliasing between subject and background -- and when
            # the model also painted a soft contact shadow, it is shadow. Left in
            # at low alpha it re-appears after the downscale as a grey rim, which
            # is the single artefact that made the first finished barrels look
            # pasted on. Below `alpha_cut` it is dropped outright.
            if alpha < alpha_cut:
                dst[x, y] = (r, g, b, 0)
                continue
            a = max(alpha, 1e-3)
            sr = int(min(255, max(0, (r - (1 - a) * br) / a)))
            sg = int(min(255, max(0, (g - (1 - a) * bg_) / a)))
            sb = int(min(255, max(0, (b - (1 - a) * bb) / a)))
            dst[x, y] = (sr, sg, sb, int(round(alpha * 255)))
    return out


# --------------------------------------------------------------------------
# Finishing
# --------------------------------------------------------------------------

def unpremultiply(rgba):
    """Undo the alpha weighting a resize introduced.

    `PIL.resize` on RGBA averages the colour channels weighted by alpha, which
    darkens edges: a pixel that is 30% opaque ends up holding 30% of the subject
    colour, and compositing it later shows a dark rim. Dividing the colour back
    out by alpha restores the true colour. Alpha itself is left alone -- that is
    the value the weighting was supposed to produce.
    """
    w, h = rgba.size
    px = rgba.load()
    out = Image.new("RGBA", (w, h))
    dst = out.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                dst[x, y] = (r, g, b, 0)
                continue
            if a == 255:
                dst[x, y] = (r, g, b, 255)
                continue
            f = 255.0 / a
            dst[x, y] = (min(255, int(r * f)), min(255, int(g * f)),
                         min(255, int(b * f)), a)
    return out


def load_palette():
    with open(PALETTE_PATH) as handle:
        return [tuple(c) for c in json.load(handle)["colors"]]


def lock_to_palette(rgba, palette):
    """Quantise the opaque pixels to the game palette, preserving alpha.

    Done by compositing over each palette colour and picking the nearest, rather
    than `Image.quantize`: quantize on an RGBA image flattens alpha, and a sprite
    whose cut-out turns black is worse than one with a few extra colours.
    """
    w, h = rgba.size
    px = rgba.load()
    out = Image.new("RGBA", (w, h))
    dst = out.load()
    cache = {}
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                dst[x, y] = (0, 0, 0, 0)
                continue
            key = (r, g, b)
            best = cache.get(key)
            if best is None:
                best = min(palette,
                           key=lambda p: (p[0] - r) ** 2 + (p[1] - g) ** 2
                           + (p[2] - b) ** 2)
                cache[key] = best
            dst[x, y] = (best[0], best[1], best[2], a)
    return out


def finish(path, size=32, tolerance=38, resample=Image.LANCZOS,
           palette=None, margin=0.06, hard=False, alpha_gain=1.0, erode=0,
           alpha_cut=0.0, isolate=True):
    """Full per-image pipeline. Returns (sprite, diagnostics dict)."""
    with Image.open(path) as raw:
        img = raw.convert("RGB")
        w, h = img.size

        mask = flood_background(img, tolerance)
        coverage = background_coverage(mask)

        # If almost nothing was removed, the render has no keyable background --
        # it is a full-frame texture. Refusing here is better than emitting a
        # sprite that is a crop of a pattern.
        if coverage < 5.0:
            return None, {"reason": f"no background found ({coverage:.1f}% keyed)",
                          "coverage": coverage}

        mask = fill_holes(mask, w, h)

        # Reduce to the single largest blob before measuring anything. Without
        # this, a painted contact shadow or a stray speck inflates the bounding
        # box to the whole frame, which is what rejected 13 of 32 perfectly good
        # renders on the first real batch.
        if isolate:
            mask, blob_share = largest_component(mask, w, h)
            if mask is None:
                return None, {"reason": f"no blob above {blob_share * 100:.1f}% "
                                        "of frame", "coverage": coverage,
                              "blob": round(blob_share * 100, 2)}
        else:
            blob_share = 1.0 - coverage / 100.0

        box = subject_box(mask, w, h)
        if box is None:
            return None, {"reason": "subject is empty after keying",
                          "coverage": coverage}

        minx, miny, maxx, maxy = box
        sw, sh = maxx - minx + 1, maxy - miny + 1
        # Two different measurements, and conflating them was a bug:
        #   `fill`  -- how much of the frame the subject's BOX covers, which is
        #              large for any diagonal or elongated object (a ladder, a
        #              signpost) and says nothing about whether keying worked;
        #   `density` -- how much of that box is actually subject. A box that is
        #              nearly all subject and spans the frame means the flood fill
        #              failed and the "object" is the whole image.
        fill = 100.0 * (sw * sh) / (w * h)
        density = 100.0 * blob_share / max(1e-6, (sw * sh) / float(w * h))

        if density > 92.0 and sw * sh > 0.9 * w * h:
            return None, {"reason": f"subject box is {density:.0f}% solid across "
                                    f"{fill:.0f}% of the frame; background was "
                                    "not separated",
                          "coverage": coverage, "fill": fill,
                          "density": round(density, 1)}

        keyed = key_out(img, mask, w, h, tolerance, hard=hard,
                        alpha_gain=alpha_gain, erode=erode, alpha_cut=alpha_cut)

        # Square crop around the subject, with a margin so it does not touch the
        # sprite edge (the real props all have a pixel or two of breathing room).
        pad_x = max(2, int(sw * margin))
        pad_y = max(2, int(sh * margin))
        cx0 = max(0, minx - pad_x)
        cy0 = max(0, miny - pad_y)
        cx1 = min(w, maxx + 1 + pad_x)
        cy1 = min(h, maxy + 1 + pad_y)
        crop = keyed.crop((cx0, cy0, cx1, cy1))
        cw, ch = crop.size

        # Square it onto a transparent canvas so proportions survive: a lamp post
        # must not be squashed into a 32x32 box.
        side = max(cw, ch)
        square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        square.paste(crop, ((side - cw) // 2, (side - ch) // 2))

        small = square.resize((size, size), resample)
        # Order matters: un-premultiply the resize's alpha weighting before the
        # palette lock, or the lock quantises darkened edge colours and bakes the
        # dark rim in permanently.
        small = unpremultiply(small)
        if palette:
            small = lock_to_palette(small, palette)

        opaque = sum(1 for p in small.getdata() if p[3] > 0)
        return small, {
            "coverage": round(coverage, 1),
            "fill": round(fill, 1),
            "density": round(density, 1),
            "blob": round(blob_share * 100, 2),
            "box": box,
            "subject_px": (sw, sh),
            "sprite_fill": round(100.0 * opaque / (size * size), 1),
            "crop": (cx0, cy0, cx1, cy1),
        }


# --------------------------------------------------------------------------
# Review output
# --------------------------------------------------------------------------

def on_game_background(sprite, bg):
    """Composite a transparent sprite onto the game's own sprite background.

    `data/sprites/` props are RGBA but are drawn over a dark field, so reviewing
    them on white lies about how they read. Take the background from the real
    art pack rather than picking one.
    """
    canvas = Image.new("RGB", sprite.size, bg)
    canvas.paste(sprite, (0, 0), sprite)
    return canvas


# Directories that hold finished sprites, never renders. Keying a sprite that is
# already transparent would produce garbage, so they are excluded from the search.
FINISHED_DIRS = ("final", "final512", "final768")


def all_renders():
    """Every candidate render under comfy_out/, recursively, sorted.

    Recursive because comfy_props.py namespaces each experiment's raws under its
    own output directory (`i2i_55/raw/`, `gen768/raw/`, ...), so a new experiment
    is picked up without editing this tool.
    """
    found = []
    for root, dirs, files in os.walk(OUT_DIR):
        dirs[:] = [d for d in dirs if d not in FINISHED_DIRS]
        if os.path.basename(root) in FINISHED_DIRS:
            continue
        for name in files:
            if not name.endswith(".png") or name.startswith("_"):
                continue
            # Contact sheets are 1000+px composites that this tool itself wrote.
            # They are not renders, and their dimensions defeat a size check.
            if name.endswith("_sheet.png") or name.endswith("_compare.png"):
                continue
            full = os.path.join(root, name)
            if is_finished_sprite(full):
                continue
            found.append(full)
    return sorted(found)


def is_finished_sprite(path, minimum=128):
    """True if this PNG is too small to be a render, i.e. a finished sprite.

    Read from the PNG header rather than decoding: this runs over every candidate
    on each invocation and only needs the dimensions. A raw render from ComfyUI is
    at least 128px on a side; anything smaller is this tool's own output, and
    re-processing it produces nonsense measurements -- a "subject" of 8% of the
    frame where the real render was 60% -- which then poisoned the curation step.
    Excluding the `final*` directory names is not enough, because an arbitrary
    `--out-dir` puts 32x32 output wherever the caller likes.
    """
    try:
        with open(path, "rb") as handle:
            header = handle.read(24)
    except OSError:
        return False
    if len(header) < 24 or header[:8] != b"\x89PNG\r\n\x1a\n":
        return False
    width = int.from_bytes(header[16:20], "big")
    height = int.from_bytes(header[20:24], "big")
    return width < minimum or height < minimum


def find_render(stem):
    """Locate a render by stem.

    Accepts a bare stem (`bucket_0`), or one qualified by its experiment
    directory (`i2i_55/bucket_0`), which is how the raw renders are actually
    laid out on disk.
    """
    want_dir = None
    want = stem
    if "/" in stem:
        want_dir, want = stem.rsplit("/", 1)
    for candidate in all_renders():
        rel = os.path.relpath(candidate, OUT_DIR)
        if want_dir and not rel.startswith(want_dir + os.sep):
            continue
        base = os.path.basename(candidate)
        bare = base[:-len("_raw.png")] if base.endswith("_raw.png") \
            else os.path.splitext(base)[0]
        if bare == want:
            return candidate
    return None


def game_background():
    for name in ("apple", "bread", "anvil"):
        path = os.path.join(GAME_DIR, name + ".png")
        if os.path.exists(path):
            with Image.open(path) as img:
                rgb = img.convert("RGB")
                # The most common colour in a real prop is its background.
                return Counter(rgb.getdata()).most_common(1)[0][0]
    return (24, 20, 28)


def contact_sheet(entries, out, zoom=10, cols=5, bg=(24, 20, 28)):
    """entries: list of (label, RGBA sprite). Also draws the real props first."""
    reals = []
    for name in ("apple", "bread", "anvil", "chest"):
        path = os.path.join(GAME_DIR, name + ".png")
        if os.path.exists(path):
            with Image.open(path) as img:
                reals.append((name, img.convert("RGBA")))

    try:
        font = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 12)
    except OSError:
        font = ImageFont.load_default()

    items = reals + list(entries)
    tile = 32 * zoom
    pad = 10
    label_h = 16
    cols = min(cols, max(1, len(items)))
    rows = (len(items) + cols - 1) // cols
    width = cols * (tile + pad) + pad
    height = rows * (tile + label_h + pad) + pad + 18
    canvas = Image.new("RGB", (width, height), (26, 26, 32))
    draw = ImageDraw.Draw(canvas)
    draw.text((pad, 4), "first 4 = real art pack (the target)", fill=(150, 150, 165),
              font=font)

    for index, (label, sprite) in enumerate(items):
        col = index % cols
        row = index // cols
        x = pad + col * (tile + pad)
        y = pad + 18 + row * (tile + label_h + pad)
        flat = on_game_background(sprite.convert("RGBA"), bg)
        canvas.paste(flat.resize((tile, tile), Image.NEAREST), (x, y))
        draw.text((x, y + tile + 2), label[:20], fill=(225, 225, 230), font=font)

    os.makedirs(os.path.dirname(out), exist_ok=True)
    canvas.save(out)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("images", nargs="*",
                    help="stems or paths; default is every raw render")
    ap.add_argument("--size", type=int, default=32)
    ap.add_argument("--tolerance", type=int, default=38,
                    help="background flood-fill tolerance (0-255 per channel)")
    ap.add_argument("--smooth", action="store_true",
                    help="LANCZOS instead of NEAREST. Almost always wrong for "
                         "pixel art: it invents in-between colours and soft "
                         "edges, which is what 'blurry but pixelated' looks "
                         "like at 32px")
    ap.add_argument("--hard-edge", action="store_true",
                    help="binary alpha cut instead of an anti-aliased matte")
    ap.add_argument("--alpha-gain", type=float, default=1.0,
                    help=">1 makes faint contact shadow transparent")
    ap.add_argument("--erode", type=int, default=0,
                    help="shrink the opaque subject by N pixels")
    ap.add_argument("--alpha-cut", type=float, default=0.0,
                    help="drop alpha below this (kills soft shadow fringe)")
    ap.add_argument("--no-isolate", action="store_true",
                    help="keep every blob, not just the largest (default: isolate)")
    ap.add_argument("--lock", action="store_true",
                    help="quantise to data/palette.json")
    ap.add_argument("--margin", type=float, default=0.06)
    ap.add_argument("--out-dir", default=FINAL_DIR)
    ap.add_argument("--tag", default="",
                    help="prefix output names, so two experiments finishing into "
                         "one directory cannot overwrite each other")
    ap.add_argument("--sheet", default=os.path.join(OUT_DIR, "final_sheet.png"))
    args = ap.parse_args()

    if args.images:
        paths = []
        for item in args.images:
            if os.path.exists(item):
                paths.append(item)
                continue
            stem = item[:-4] if item.endswith(".png") else item
            found = find_render(stem)
            if not found:
                sys.exit(f"cannot find a render for {item!r} under {OUT_DIR}")
            paths.append(found)
    else:
        paths = all_renders()

    if not paths:
        sys.exit("nothing to finish. Generate first, or name a render.")

    palette = load_palette() if args.lock else None
    # NEAREST by default. This was originally LANCZOS, chosen to match the soft
    # shading of the real art pack -- and it was the wrong call, caught by looking
    # at the output at 1x rather than at 6x. Lanczos averaging turns a 128px
    # pixel-art grid into dithered mush at 32px. The real sprites have soft
    # *shading* but perfectly hard *edges*, and NEAREST is what preserves those.
    resample = Image.LANCZOS if args.smooth else Image.NEAREST
    os.makedirs(args.out_dir, exist_ok=True)
    bg = game_background()

    print(f"{'render':40s} {'keyed%':>7s} {'blob%':>6s} {'fill%':>6s} "
          f"{'dens%':>6s} {'subject':>11s} {'sprite%':>8s}  result")
    print("-" * 112)
    entries = []
    accepted = 0
    for path in paths:
        # Raw renders are named "<stem>_raw.png", the comfy_out copies "<stem>.png";
        # both should land on the same output name.
        base = os.path.basename(path)
        stem = base[:-len("_raw.png")] if base.endswith("_raw.png") \
            else os.path.splitext(base)[0]
        if args.tag:
            stem = f"{args.tag}_{stem}"
        try:
            sprite, info = finish(path, args.size, args.tolerance, resample,
                                  palette, args.margin, args.hard_edge,
                                  args.alpha_gain, args.erode, args.alpha_cut,
                                  not args.no_isolate)
        except Exception as exc:
            print(f"{stem[:40]:40s} {'':7s} {'':6s} {'':11s} {'':8s}  ERROR {exc}")
            continue

        if sprite is None:
            print(f"{stem[:40]:40s} {info.get('coverage', 0):7.1f} "
                  f"{info.get('blob', 0):6.1f} {info.get('fill', 0):6.1f} "
                  f"{info.get('density', 0):6.1f} {'':11s} {'':8s}  REJECT "
                  f"{info['reason']}")
            continue

        accepted += 1
        out_path = os.path.join(args.out_dir, f"{stem}.png")
        sprite.save(out_path)
        entries.append((stem, sprite))
        print(f"{stem[:40]:40s} {info['coverage']:7.1f} {info['blob']:6.1f} "
              f"{info['fill']:6.1f} {info['density']:6.1f} "
              f"{str(info['subject_px']):>11s} {info['sprite_fill']:8.1f}  ok -> "
              f"{os.path.relpath(out_path, ROOT)}")

    print(f"\n{accepted}/{len(paths)} accepted into {os.path.relpath(args.out_dir, ROOT)}")
    if entries:
        sheet = contact_sheet(entries, args.sheet)
        print(f"wrote {os.path.relpath(sheet, ROOT)}")
        print("now put one on a map in the editor and look at it with real "
              "tiles around it (tools/README.md §4).")


if __name__ == "__main__":
    main()
