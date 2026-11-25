#!/usr/bin/env python3
"""Turn one pixel-art reference image into a .sprite reference and a style draft.

This is the "reference -> style" half of the art-style pipeline. It is deliberately
deterministic: crop the image to one character, key out the flat background, trim,
resize to the target sprite size, quantise to a small palette, and emit the result as
a `.sprite` document the DSL compiler (and the model) can read. The companion style
JSON is a *draft*: the palette and the rough part proportions are measured; the prose
is a starting point that the user is expected to sharpen.

Usage:
  nix-shell --run "python3 tools/style_from_image.py \\
      --input /tmp/style_ref.jpg --crop 900,120,360,760 \\
      --name style_ref_green_shaman --style-id cheeseguy_pixel \\
      --size 96 --colors 24"
"""

from __future__ import annotations

import argparse
import json
import os
from collections import Counter

from PIL import Image


def parse_crop(value: str) -> tuple[int, int, int, int]:
    parts = [int(p) for p in value.split(",")]
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("crop must be x,y,w,h")
    return tuple(parts)  # type: ignore[return-value]


def parse_hex(value: str) -> tuple[int, int, int]:
    value = value.lstrip("#")
    if len(value) == 3:
        value = "".join(c * 2 for c in value)
    if len(value) != 6:
        raise argparse.ArgumentTypeError("colour must be hex, e.g. 6b4a5a")
    return tuple(int(value[i : i + 2], 16) for i in (0, 2, 4))  # type: ignore[return-value]


def border_key(image: Image.Image) -> tuple[int, int, int]:
    """The most common colour on the crop's border; the sheet's flat backdrop."""
    width, height = image.size
    pixels: list[tuple[int, int, int]] = []
    for x in range(width):
        pixels.append(image.getpixel((x, 0))[:3])
        pixels.append(image.getpixel((x, height - 1))[:3])
    for y in range(height):
        pixels.append(image.getpixel((0, y))[:3])
        pixels.append(image.getpixel((width - 1, y))[:3])
    return Counter(pixels).most_common(1)[0][0]


def key_background(image: Image.Image, key: tuple[int, int, int], tolerance: int) -> Image.Image:
    """Make pixels close to `key` transparent. The sheet is flat; a colour distance
    key keeps the dark outline and the anti-aliased edge pixels that are far from it."""
    pixels = image.load()
    width, height = image.size
    limit = tolerance * tolerance
    for y in range(height):
        for x in range(width):
            r, g, b, a = pixels[x, y]
            dr, dg, db = r - key[0], g - key[1], b - key[2]
            if dr * dr + dg * dg + db * db <= limit:
                pixels[x, y] = (r, g, b, 0)
    return image


def trim_to_opaque(image: Image.Image) -> Image.Image:
    bbox = image.getbbox()
    return image.crop(bbox) if bbox else image


def fit_to_canvas(image: Image.Image, size: int) -> Image.Image:
    """Scale so the character fills the height, keeping the feet at the bottom edge.
    NEAREST keeps the pixel-art grid rather than introducing soft new colours."""
    width, height = image.size
    scale = min(size / height, size / width)
    new_size = (max(1, round(width * scale)), max(1, round(height * scale)))
    resized = image.resize(new_size, Image.Resampling.NEAREST)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.paste(resized, ((size - new_size[0]) // 2, size - new_size[1]), resized)
    return canvas


def quantize(image: Image.Image, colours: int) -> tuple[Image.Image, list[tuple[int, int, int]]]:
    alpha = image.getchannel("A")
    rgb = image.convert("RGB")
    quantized = rgb.quantize(colors=colours, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    quantized = quantized.convert("RGBA")
    quantized.putalpha(alpha)
    palette = quantized.getpalette() or []
    used: Counter[tuple[int, int, int]] = Counter()
    pixels = quantized.load()
    width, height = quantized.size
    for y in range(height):
        for x in range(width):
            r, g, b, a = pixels[x, y]
            if a > 0:
                used[(r, g, b)] += 1
    ordered = [colour for colour, _ in used.most_common()]
    return quantized, ordered


def emit_sprite(image: Image.Image, name: str) -> str:
    """Horizontal-run-length a transparent-background sprite into the DSL."""
    width, height = image.size
    pixels = image.load()
    lines = [f"version 1", f"sprite {width}x{height}", "kind object", "", 'frame "f1" {']
    for y in range(height):
        x = 0
        while x < width:
            r, g, b, a = pixels[x, y]
            if a == 0:
                x += 1
                continue
            start = x
            while x + 1 < width:
                nr, ng, nb, na = pixels[x + 1, y]
                if na == 0 or (nr, ng, nb) != (r, g, b):
                    break
                x += 1
            end = x
            colour = f"#{r:02x}{g:02x}{b:02x}"
            if start == end:
                lines.append(f"  pixel ({start},{y}) color={colour}")
            else:
                lines.append(f"  rect ({start},{y}) ({end},{y}) color={colour}")
            x += 1
    lines.append("}")
    return "\n".join(lines) + "\n"


def geometry(size: int) -> dict:
    """Rough humanoid boxes for the sheet's 4-5 head proportions. The user is expected
    to adjust these in the Content Editor once the style test shows what the model does."""
    def box(x, y, w, h):
        return [round(x * size), round(y * size), round(w * size), round(h * size)]

    return {
        "width": size,
        "height": size,
        "head": box(0.34, 0.02, 0.32, 0.26),
        "hair": box(0.31, 0.00, 0.38, 0.22),
        "body": box(0.30, 0.28, 0.40, 0.26),
        "arm_left": box(0.22, 0.30, 0.09, 0.26),
        "arm_right": box(0.69, 0.30, 0.09, 0.26),
        "legs": box(0.34, 0.54, 0.32, 0.42),
        "hand": [round(0.24 * size), round(0.58 * size)],
        "foot": [round(0.50 * size), round(0.98 * size)],
        "base": [round(0.46 * size), round(0.20 * size)],
        "pick": [round(0.80 * size), round(0.74 * size)],
    }


def style_draft(args, palette: list[tuple[int, int, int]], reference: str, size: int) -> dict:
    hexes = " ".join(f"#{r:02x}{g:02x}{b:02x}" for r, g, b in palette[:16])
    return {
        "id": args.style_id,
        "name": args.style_name or args.style_id.replace("_", " ").title(),
        "description": "Pixel-art character style derived from a reference sheet.",
        "appliesTo": ["actor"],
        "palette": hexes,
        "geometry": geometry(size),
        "positive": (
            "Detailed Ragnarok-like pixel-art chibi, roughly four to five heads tall, "
            "long legs and a readable torso rather than a giant head. Dark near-black "
            "outline around the silhouette and interior line work. Four or five soft "
            "pixel shading bands per material (skin, hair, cloth, metal, leather), with "
            "highlight and shadow clusters rather than flat fills. Muted, slightly "
            "desaturated base colours with a few strong accents. Hair in strands and "
            "chunky locks, armor/accessories with readable plates and trim, feet planted "
            "at the bottom of the canvas. Crisp one-pixel details, no anti-aliasing."
        ),
        "negative": (
            "no flat single-colour fills, no missing or light outline, no one-to-one "
            "super-deformed head/body ratio, no pure black, no soft anti-aliased edges, "
            "no high-saturation candy palette, no generic blank face, no chunky 32px "
            "proportions."
        ),
        "reference": reference,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="reference image (png/jpg)")
    parser.add_argument("--crop", type=parse_crop, default=None, help="x,y,w,h")
    parser.add_argument("--background", type=parse_hex, default=None, help="flat backdrop hex")
    parser.add_argument("--tolerance", type=int, default=26)
    parser.add_argument("--size", type=int, default=96, help="square sprite canvas")
    parser.add_argument("--colors", type=int, default=24)
    parser.add_argument("--name", required=True, help="sprite/document id")
    parser.add_argument("--style-id", default=None)
    parser.add_argument("--style-name", default=None)
    parser.add_argument("--sprite-out", default=None)
    parser.add_argument("--style-out", default=None)
    args = parser.parse_args()

    args.style_id = args.style_id or (args.name + "_style")
    image = Image.open(args.input).convert("RGBA")
    if args.crop:
        left, upper, width, height = args.crop
        image = image.crop((left, upper, left + width, upper + height))
    key = args.background or border_key(image)
    image = key_background(image, key, args.tolerance)
    image = trim_to_opaque(image)
    image = fit_to_canvas(image, args.size)
    image, palette = quantize(image, args.colors)

    sprite_path = args.sprite_out or os.path.join(
        "packs/generated/village/sprites", args.name + ".sprite"
    )
    os.makedirs(os.path.dirname(sprite_path), exist_ok=True)
    with open(sprite_path, "w", encoding="utf-8") as handle:
        handle.write(emit_sprite(image, args.name))

    style_path = args.style_out or os.path.join("data/art_styles", args.style_id + ".json")
    os.makedirs(os.path.dirname(style_path), exist_ok=True)
    with open(style_path, "w", encoding="utf-8") as handle:
        json.dump(style_draft(args, palette, args.name, args.size), handle, indent=2)
        handle.write("\n")

    print(f"wrote {sprite_path} ({len(palette)} colours) and {style_path}")
    print("palette:", " ".join(f"#{r:02x}{g:02x}{b:02x}" for r, g, b in palette[:16]))


if __name__ == "__main__":
    main()
