#!/usr/bin/env python3
"""Build a LoRA training set for an art pack from the pack's own source art.

Stage 7 of docs/asset-generation-design.md; see section 8.1 for why the selection
is what it is.

The one decision that matters: train on the SOURCE art at its own resolution, never
on the 32x32 crops the game draws. `data/sprites/` is 57 files, all 32px, and they
are not one artist's hand -- some came out of a Gemini call, some from a separate
lineup call. A LoRA learns the distribution of appearance it is shown, so training
on that folder teaches a mixture, at a resolution where the base model cannot see
the texture it is supposed to learn.

What this does:

  * walks a source pack for images
  * crops each to the bounding box of its opaque pixels, and drops the ones with
    no opaque pixels at all
  * keeps those whose SHORTER side is at least --min-side (192 by default), which
    drops the narrow animation strips -- they teach motion frames, not style
  * writes them into a flat dataset directory under stable numbered names
  * writes one caption file per image containing ONE trigger token and nothing else

Why cropping is not optional in practice. Measured on this pack: the art is
transparent-padded, `Shadows.png` and `Buildings/Shadows.png` are 0.0% opaque
(entirely empty), `Icons/Resources.png` is 400x400 with a 400x112 content box at
1.8% opaque, and the content boxes are 66% of the raw pixels overall. A trainer
sees whatever is in the file, so an uncropped set spends capacity on flat
background and includes two images that are nothing but background.

Why one trigger and no content words. A single token shared by every image leaves
the model no content words to bind appearance to, so the trigger collects *style*:
palette, outline weight, shading idiom, dithering. Captioning content instead
("a stone wall, a wooden bench") teaches the trigger to mean many unrelated things
and dilutes exactly what is wanted. Section 8.2.

Usage:
  python3 tools/lora_dataset.py --source "data/tilesets/Pixel Crawler - Free Pack" \
      --out packs/art/pixel_crawler/lora/train/dataset --dry-run
"""

import argparse
import os
import sys
from collections import Counter

try:
    from PIL import Image
except ImportError:  # pragma: no cover - environment problem, not a code path
    sys.stderr.write(
        "lora_dataset.py needs Pillow. On NixOS the system python cannot see it:\n"
        '  nix-shell -p python3Packages.pillow --run "python3 tools/lora_dataset.py ..."\n'
    )
    raise SystemExit(2)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# One token, shared by every image. The name is arbitrary; that it is unique and
# used consistently is not -- it is the handle the prompt uses to invoke the style.
DEFAULT_TRIGGER = "pxcrawler_pixel_art"

# Shortest content side worth training on. 192 rather than SD 1.5's 256 floor
# because this pack's art is sparse: measured against the content boxes, 256 keeps
# 27 images and 192 keeps 36, and the extra nine are real structures and props
# (rocks, tree models, a furnace, a workbench) rather than animation frames. Pair it
# with min_bucket_reso=192 so those images train at their own scale instead of being
# upscaled to reach a 256 bucket.
DEFAULT_MIN_SIDE = 192


def sanitise(name):
    """A filesystem-safe stem, keeping enough path to stay unique."""
    out = []
    for ch in name:
        out.append(ch if ch.isalnum() or ch in "-_" else "_")
    slug = "".join(out)
    while "__" in slug:
        slug = slug.replace("__", "_")
    return slug.strip("_")


def collect(source, min_side, max_images, crop=True):
    """(kept, skipped) where each entry carries its path, bbox and size.

    Sizing is measured AFTER cropping to the opaque bounding box, and that order
    matters more than it looks. Measured on this pack: the atlas pages are sparse
    canvases -- `Icons/Resources.png` is 400x400 with a 400x112 content box at 1.8%
    opaque, `Furniture.png` is 800x864 at 8.9% -- and `Shadows.png` and
    `Buildings/Shadows.png` are 0.0% opaque, i.e. entirely empty. A trainer sees
    whatever is in the file, so an unfiltered set would spend most of its capacity
    on flat background, and two of its images would be pure background.

    Cropping first also makes the filter honest: a sparse 400x400 page is not a
    400x400 example, it is whatever its art actually occupies, and it should live
    or die on that size.
    """
    kept = []
    skipped = Counter()
    for directory, _dirs, files in os.walk(source):
        for name in sorted(files):
            if not name.lower().endswith(".png"):
                continue
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, source)
            try:
                with Image.open(path) as image:
                    rgba = image.convert("RGBA")
                    width, height = rgba.size
                    bbox = rgba.getchannel("A").getbbox()
                    if bbox is None:
                        # Fully transparent. Nothing here to learn from, and after
                        # compositing it is a black square.
                        skipped["entirely empty (no opaque pixels)"] += 1
                        continue
                    opaque = sum(1 for a in rgba.getchannel("A").getdata() if a >= 128)
            except Exception as error:  # unreadable file: report, do not fail
                skipped["unreadable"] += 1
                print(f"  skip unreadable: {relative} ({error})")
                continue

            # The *sheet* variants of an animation are the same pixels as the plain
            # one, so keeping both would double-count a single loop of frames.
            if name.endswith("-Sheet.png"):
                skipped["duplicate -Sheet"] += 1
                continue

            box = bbox if crop else (0, 0, width, height)
            crop_w, crop_h = box[2] - box[0], box[3] - box[1]
            if min(crop_w, crop_h) < min_side:
                skipped[f"content shorter side < {min_side}"] += 1
                continue

            kept.append({"path": path, "relative": relative, "box": box,
                         "width": crop_w, "height": crop_h,
                         "full": (width, height), "opaque": opaque})

    # Largest first: with a cap, the biggest pages carry the most style per image.
    kept.sort(key=lambda e: -(e["width"] * e["height"]))
    if max_images and len(kept) > max_images:
        skipped[f"beyond --max-images {max_images}"] += len(kept) - max_images
        kept = kept[:max_images]
    return kept, skipped


def build(kept, out_dir, trigger, contact_sheet):
    os.makedirs(out_dir, exist_ok=True)
    # Clear a previous selection first. Without this a rebuild that keeps fewer
    # images leaves the surplus behind, and the trainer silently learns from a
    # dataset that is the union of two runs -- which is exactly the kind of wrong
    # that shows up as a bad adapter rather than as an error.
    stale = 0
    for name in os.listdir(out_dir):
        if name.endswith(".png") or name.endswith(".txt"):
            os.remove(os.path.join(out_dir, name))
            stale += 1
    if stale:
        print(f"cleared    : {stale} file(s) from a previous selection")

    manifest = []
    for index, entry in enumerate(kept):
        stem = f"{index:03d}_{sanitise(entry['relative'][:-4])}"
        destination = os.path.join(out_dir, stem + ".png")
        with Image.open(entry["path"]) as image:
            cropped = image.convert("RGBA").crop(entry["box"])
            # Alpha is kept rather than composited. Cropping has already removed
            # most of the padding, so the remaining transparency is the gaps
            # between sprites, and what a trainer does with it is its own setting
            # rather than something to bake in here.
            cropped.save(destination)
        # One token, nothing else.
        with open(os.path.join(out_dir, stem + ".txt"), "w") as handle:
            handle.write(trigger + "\n")
        manifest.append({"dataset": stem, "source": entry["relative"],
                         "size": f"{entry['width']}x{entry['height']}",
                         "original": f"{entry['full'][0]}x{entry['full'][1]}"})

    with open(os.path.join(out_dir, "manifest.txt"), "w") as handle:
        handle.write(f"source images : {len(kept)}\n")
        handle.write(f"trigger       : {trigger}\n")
        handle.write("sizes are content-cropped; 'from' is the original canvas\n\n")
        for row in manifest:
            handle.write(f"{row['dataset']:52s} <- {row['source']} "
                         f"({row['size']} from {row['original']})\n")

    if contact_sheet:
        write_contact_sheet(kept, contact_sheet)
    return manifest


def write_contact_sheet(kept, path):
    """Thumbnails of the whole training set, as the trainer will see it.

    Cropped, and composited onto mid grey rather than black. Black was actively
    misleading on the first run of this: the pack's art is transparent-padded, so
    flattening it to black made sparse pages look like empty black squares and hid
    the fact that two of them were entirely empty.
    """
    thumbs = []
    for entry in kept:
        with Image.open(entry["path"]) as image:
            cropped = image.convert("RGBA").crop(entry["box"])
            flat = Image.new("RGB", cropped.size, (128, 128, 128))
            flat.paste(cropped, (0, 0), cropped)
            scale = 96 / max(flat.size)
            size = (max(1, int(flat.width * scale)), max(1, int(flat.height * scale)))
            thumbs.append(flat.resize(size, Image.LANCZOS))
    if not thumbs:
        return

    per_row = 8
    cell = 104
    rows = (len(thumbs) + per_row - 1) // per_row
    sheet = Image.new("RGB", (per_row * cell, rows * cell), (24, 24, 28))
    for index, thumb in enumerate(thumbs):
        x = (index % per_row) * cell + (cell - thumb.width) // 2
        y = (index // per_row) * cell + (cell - thumb.height) // 2
        sheet.paste(thumb, (x, y))
    sheet.save(path)
    print(f"contact sheet: {os.path.relpath(path, ROOT)} "
          f"({len(thumbs)} images, {per_row} per row)")


def display_path(path):
    absolute = os.path.abspath(path)
    root = os.path.abspath(ROOT) + os.sep
    return absolute[len(root):] if absolute.startswith(root) else absolute


def main():
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", default="data/tilesets/Pixel Crawler - Free Pack",
                        help="the art pack's own source art, at full resolution")
    parser.add_argument("--out", default="packs/art/pixel_crawler/lora/train/dataset",
                        help="where the flat dataset and its captions go")
    parser.add_argument("--trigger", default=DEFAULT_TRIGGER,
                        help=f"the single caption token (default {DEFAULT_TRIGGER})")
    parser.add_argument("--min-side", type=int, default=DEFAULT_MIN_SIDE,
                        help=f"shortest side to keep, measured AFTER cropping to "
                             f"content (default {DEFAULT_MIN_SIDE})")
    parser.add_argument("--no-crop", action="store_true",
                        help="keep the original canvas instead of cropping to the "
                             "opaque bounding box. Cropping is on by default because "
                             "these atlas pages are sparse: measured, the content box "
                             "is 66%% of the raw pixels overall, and two pages are "
                             "entirely empty")
    parser.add_argument("--max-images", type=int, default=80,
                        help="cap, largest-first (default 80). A style adapter wants "
                             "30-100 images; more than that mostly adds training time")
    parser.add_argument("--contact-sheet", default="",
                        help="also write a thumbnail grid of the whole set, to check "
                             "it looks like one artist before spending GPU time")
    parser.add_argument("--dry-run", action="store_true",
                        help="report the selection and write nothing")
    args = parser.parse_args()

    source = args.source
    if not os.path.isdir(source):
        sys.exit(f"no source directory at {source}")

    kept, skipped = collect(source, args.min_side, args.max_images,
                           crop=not args.no_crop)

    print(f"source     : {display_path(source)}")
    print(f"trigger    : {args.trigger}")
    print(f"min side   : {args.min_side}px, {'after cropping to content' if not args.no_crop else 'uncropped'}")
    print(f"kept       : {len(kept)} images")
    if kept:
        print(f"             {min(e['width'] for e in kept)}-"
              f"{max(e['width'] for e in kept)}px wide, "
              f"{sum(e['width'] * e['height'] for e in kept) / 1e6:.1f} megapixels")
        # The number that matters for a style adapter: how much of what the trainer
        # sees is actually art. Low values mean capacity spent on background.
        opaque = sum(e["opaque"] for e in kept)
        area = sum(e["width"] * e["height"] for e in kept)
        print(f"             {100 * opaque / area:.0f}% of those pixels are opaque art")
    for reason, count in sorted(skipped.items()):
        print(f"skipped    : {count:4d}  {reason}")

    if not kept:
        sys.exit("nothing qualified -- lower --min-side?")

    if len(kept) < 30:
        print(f"\nNOTE: {len(kept)} images is below the 30-100 a style adapter wants. "
              f"Lower --min-side to reach more.")

    if args.dry_run:
        print(f"\ndry run: would write {len(kept)} images + captions to "
              f"{display_path(args.out)}")
        for entry in kept[:5]:
            print(f"  {entry['width']:4d}x{entry['height']:<4d} {entry['relative']}")
        if len(kept) > 5:
            print(f"  ... and {len(kept) - 5} more")
        return

    manifest = build(kept, args.out, args.trigger, args.contact_sheet)
    print(f"\nwrote {len(manifest)} images + captions to {display_path(args.out)}")
    print(f"      manifest: {display_path(os.path.join(args.out, 'manifest.txt'))}")


if __name__ == "__main__":
    main()
