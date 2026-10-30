#!/usr/bin/env python3
"""Derive the art pack's material ramps into data/style/ramps.json.

The sprite DSL addresses colour by *role*, not by value: a document says
`wood.base` or `wood.3`, never `#b3692f`. Something has to decide what "wood"
is, and the answer must come from the art pack, not from a model's taste -- that
is the whole reason generated art can look like it belongs (see
docs/pixel-dsl.md, "Where colour comes from").

So this reads the pack's own sprites, groups their colours by material, splits
each group by *luminance* into `steps` bands, and keeps the pack's own tones
verbatim.

**Why ramps are not snapped to data/palette.json.** That file is a 128-colour
approximation of the pack, not the pack's palette: tile_grass is only 65% in it,
and snapping a grass ramp to it collapses four distinct tones into one. A DSL
document cannot invent a colour at all -- it can only name ramp entries -- so
"every pixel is a colour the pack already uses" holds by construction and is a
stronger guarantee than "every pixel is near a 128-colour approximation". The
palette report below is informational for that reason.

Two things that are easy to get wrong and are deliberate here:

  * **Bands are luminance ranges, not pixel-count quantiles.** The first version
    bucketed by cumulative pixel count and every ramp collapsed to one colour:
    a tile is 65% one tone, so that tone filled the first two buckets and the
    remaining bands had nothing left. Luminance ranges do not care how popular a
    tone is.
  * **The registry is hand-written.** `tile_floor` is stone and `tile_path` is
    sand because a person said so; guessing "which sprite is which material" from
    a filename is the same heuristic that put anvils through the tile pipeline.
    The anvil is also mostly a *wooden stump* around an iron head, so "iron"
    filters to near-grey colours or the ramp comes back orange.

    nix-shell -p python3Packages.pillow --run \\
        "python3 tools/derive_ramps.py --out data/style/ramps.json"

Re-run it when the art pack changes; the output is committed because both the
compiler and the designer prompt read it.
"""

import argparse
import json
import os
import sys
from collections import Counter

from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# material -> (sprites, steps, colour filter). Steps run dark -> light, so
# `<material>.0` is the shadow and the last entry is the highlight. Five is
# enough for a 32px surface: base, two shadow steps, two light steps.
MATERIALS = {
    "stone": (["tile_floor"], 5, None),
    "sand": (["tile_path"], 5, None),
    "grass": (["tile_grass"], 5, None),
    "soil": (["tile_tilled_soil", "tile_packed_earth"], 5, None),
    "wood": (["tile_wooden_floor"], 5, None),
    "water": (["tile_water"], 4, None),
    "iron": (["anvil"], 5, "grey"),
}

# The object palette's own contribution: the outline every prop is drawn with.
# One step is right -- an outline is a colour, not a ramp.
INK_SOURCES = ["chest"]

# A colour needs this share of the sprite to be part of its ramp. Below it the
# colour is anti-aliasing or a stray pixel and averaging it in muddies the band.
MIN_SHARE = 0.002


def luminance(rgb):
    # Rec. 601: what reads as darker to the eye, which is what the bands are for.
    return 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]


def is_grey(rgb):
    return max(rgb) - min(rgb) <= 24


def load_sprites(names, keep=None):
    colours = Counter()
    pixels = 0
    for name in names:
        path = os.path.join(REPO, "data", "sprites", name + ".png")
        if not os.path.exists(path):
            print("WARNING: missing sprite " + path, file=sys.stderr)
            continue
        im = Image.open(path).convert("RGBA")
        for r, g, b, a in im.getdata():
            if a == 0:
                continue
            if keep is not None and not keep((r, g, b)):
                continue
            colours[(r, g, b)] += 1
            pixels += 1
    return colours, pixels


def ramp_bands(colours, steps):
    """Split the colour set by luminance into `steps` bands, average each band.

    The band range is a *weighted percentile* (2%..98%), not min..max: one stray
    highlight pixel otherwise stretches the range until every real tone piles into
    the bottom band and the ramp collapses somewhere in the middle.
    """
    total = sum(colours.values())
    kept = [(rgb, count) for rgb, count in colours.items()
            if count >= MIN_SHARE * total]
    if not kept:
        kept = list(colours.items())

    ordered = sorted(kept, key=lambda kv: luminance(kv[0]))

    def percentile(fraction):
        target = total * fraction
        running = 0
        for rgb, count in ordered:
            running += count
            if running >= target:
                return luminance(rgb)
        return luminance(ordered[-1][0])

    low, high = percentile(0.02), percentile(0.98)
    if high - low < 1.0:
        return [ordered[len(ordered) // 2][0]] * steps

    bands = [[0, 0, 0, 0] for _ in range(steps)]  # r, g, b, weight
    for rgb, count in kept:
        lum = luminance(rgb)
        if lum < low or lum > high:
            # Outside the trimmed range: leave it out rather than clamping it into
            # an edge band, where it would drag that band's average.
            continue
        index = int((lum - low) / (high - low) * steps)
        index = max(0, min(steps - 1, index))
        band = bands[index]
        band[0] += rgb[0] * count
        band[1] += rgb[1] * count
        band[2] += rgb[2] * count
        band[3] += count

    def average(band):
        return (band[0] // band[3], band[1] // band[3], band[2] // band[3])

    ramp = []
    for index, band in enumerate(bands):
        if band[3]:
            ramp.append(average(band))
            continue
        # Empty band: interpolate between the nearest populated neighbours, on the
        # *averages*. (Averaging the raw sums was the first version's bug: it
        # produced three-digit-per-channel values that snapped to the palette's
        # brightest entry and painted a cream step into the grass ramp.)
        before = next((bands[i] for i in range(index - 1, -1, -1) if bands[i][3]), None)
        after = next((bands[i] for i in range(index + 1, steps) if bands[i][3]), None)
        if before and after:
            a, b = average(before), average(after)
            ramp.append(tuple((a[c] + b[c]) // 2 for c in range(3)))
        elif before or after:
            ramp.append(average(before or after))
        else:
            ramp.append(ordered[len(ordered) // 2][0])
    return ramp


def nearest(palette, rgb):
    best = palette[0]
    best_d = None
    for candidate in palette:
        d = ((candidate[0] - rgb[0]) ** 2 + (candidate[1] - rgb[1]) ** 2 +
             (candidate[2] - rgb[2]) ** 2)
        if best_d is None or d < best_d:
            best_d, best = d, candidate
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(REPO, "data", "style", "ramps.json"))
    ap.add_argument("--palette", default=os.path.join(REPO, "data", "palette.json"))
    args = ap.parse_args()

    palette = [tuple(c[:3]) for c in json.load(open(args.palette))["colors"]]
    palette_set = set(palette)
    print("palette: %d entries (report only -- ramps keep the pack's own tones)" % len(palette))

    ramps = {}
    for material, (sprites, steps, filter_name) in MATERIALS.items():
        keep = is_grey if filter_name == "grey" else None
        colours, pixels = load_sprites(sprites, keep)
        if not colours:
            print("WARNING: no colours for material " + material, file=sys.stderr)
            continue
        raw = ramp_bands(colours, steps)
        base = colours.most_common(1)[0][0]
        distinct = len({tuple(c) for c in raw})
        in_pal = sum(1 for c in raw if c in palette_set)
        print("  %-6s %-30s pixels=%5d distinct=%3d steps=%d unique=%d in_palette=%d/%d"
              % (material, ",".join(sprites), pixels, len(colours), steps,
                 distinct, in_pal, steps))
        ramps[material] = {
            "steps": [list(c) for c in raw],
            "base": list(base),
            "source": sprites,
            "pixels": pixels,
        }

    # The outline colour: the darkest tone the pack uses on props. Objects are
    # outlined in the pack's own near-black, and a generated object without one
    # reads as pasted on (docs/pixel-dsl.md).
    ink, ink_pixels = load_sprites(INK_SOURCES)
    tot = sum(ink.values())
    darkest_share = [(rgb, count) for rgb, count in ink.items()
                     if luminance(rgb) < 40 and count >= MIN_SHARE * tot]
    if darkest_share:
        weight = sum(c for _, c in darkest_share)
        ink_rgb = tuple(sum(rgb[i] * c for rgb, c in darkest_share) // weight
                        for i in range(3))
    else:
        ink_rgb = min(ink, key=luminance)
    ramps["ink"] = {"steps": [list(ink_rgb)],
                    "base": list(ink_rgb),
                    "source": INK_SOURCES, "pixels": ink_pixels}
    print("  ink    %-30s pixels=%5d steps=1 base=%s"
          % (",".join(INK_SOURCES), ink_pixels, list(ink_rgb)))

    document = {
        "version": 1,
        "generatedBy": "tools/derive_ramps.py",
        "palette": os.path.relpath(args.palette, REPO),
        "note": ("Ramps are ordered dark -> light and hold the pack's own tones, not a "
                 "palette approximation: a document can only name ramp entries, so it "
                 "cannot invent a colour the pack does not use. `base` is the material's "
                 "dominant tone. Regenerate after the art pack changes."),
        "ramps": ramps,
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as handle:
        json.dump(document, handle, indent=2)
        handle.write("\n")
    print("wrote " + os.path.relpath(args.out, REPO))


if __name__ == "__main__":
    main()
