#!/usr/bin/env python3
"""Generate a tile sprite for AIRogue with a local SD 1.5 + pixel-art LoRA.

This is a design-time tool. It writes straight into data/sprites/ (and optionally
data/tiles/), so a generated tile is immediately usable in the game and visible in
the map editor. Nothing here runs at game runtime.

Why the script rather than the ComfyUI graph: the answer this is meant to produce
is "does generated pixel art look right next to the existing art", and that lives
in the post-processing -- palette quantisation and nearest-neighbour downscale --
not in the diffusion settings. Those steps have to run every time, so they belong
in the same program as the generation.

The pipeline, and why each step is there:

  1. SD 1.5 at 512x512. Never generate at 32: diffusion at low resolution produces
     mush, not pixels.
  2. Palette quantisation to a small number of colours. This is what makes output
     from different prompts and different runs look like one artist drew them.
  3. Downscale to the tile size with NEAREST. Bilinear is what produces the
     blurry "not quite pixel art" look.
  4. Composite a contact sheet next to sprites from the game, at true game scale.
     A texture can look fine in an image viewer and unusable in a field.

See tools/README.md for the NixOS setup.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Repo root: this file lives in <root>/tools/.
REPO_ROOT = Path(__file__).resolve().parent.parent
SPRITES_DIR = REPO_ROOT / "data" / "sprites"
TILES_DIR = REPO_ROOT / "data" / "tiles"

# The tiles the contact sheet compares against. These are hand-authored or
# hand-picked art, so they are the quality bar a generated tile has to meet --
# comparing against ASCII or against nothing tells you very little.
REFERENCE_TILES = ["tile_grass", "tile_wooden_wall", "tile_tilled_soil"]

# Sensible starting point for a tile. Every term is doing work: "seamless
# tileable" and "top-down" are the two that most affect whether a texture is
# usable in a field, and "limited palette" keeps the model near something
# quantisable. The LoRA's own trigger words are prepended separately, because
# omitting them is the most common reason a pixel-art LoRA appears to do nothing.
PROMPT_TEMPLATE = (
    "{trigger}, {material}, seamless tileable texture, top-down view, "
    "limited palette, flat colours, crisp pixels, no shadow"
)

NEGATIVE_PROMPT = (
    "blurry, smooth gradient, anti-aliased, 3d render, photorealistic, "
    "isometric, perspective, text, watermark, signature, frame, border"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a game tile sprite with SD 1.5 + a pixel-art LoRA.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("material", nargs="?", help="what the tile is, e.g. grass, mossy stone")
    parser.add_argument("--model", default="",
                        help="HuggingFace repo id, or a path to a local .safetensors. "
                             "Empty means: find it under --comfy-data")
    parser.add_argument("--lora", default="",
                        help="path to a pixel-art LoRA .safetensors. Empty means: find "
                             "it under --comfy-data, or run with no LoRA")
    parser.add_argument("--comfy-data", default="",
                        help="a ComfyUI data directory to search for models "
                             "(default: $COMFY_DATA, /data, ~/comfyui-data)")
    parser.add_argument("--trigger", default="pixel art",
                        help="the LoRA's trigger words, from its model page")
    parser.add_argument("--lora-scale", type=float, default=0.8,
                        help="LoRA strength; too high destroys the texture")
    parser.add_argument("--count", type=int, default=8,
                        help="how many to generate; you will reject most of them")
    parser.add_argument("--tile-size", type=int, default=32,
                        help="output tile size in pixels; must match the game's tile size")
    parser.add_argument("--colors", type=int, default=16,
                        help="palette size for quantisation")
    parser.add_argument("--palette", default="data/palette.json",
                        help="shared pack palette from tools/palette.py derive. "
                             "When it exists every tile is snapped to it instead "
                             "of being quantised to a palette of its own")
    parser.add_argument("--steps", type=int, default=24)
    parser.add_argument("--cfg", type=float, default=7.0)
    parser.add_argument("--seed", type=int, default=1234,
                        help="fixed by default, so changing one word shows what it did")
    parser.add_argument("--name", default="",
                        help="output name stem (default: slug of the material)")
    parser.add_argument("--raw-dir", default="/tmp/air_tile_raw",
                        help="where the unprocessed 512px renders go, for diagnosis")
    parser.add_argument("--emit-tile-json", action="store_true",
                        help="also write data/tiles/<name>.json so the tile is selectable")
    parser.add_argument("--blocks-tile", action="store_true",
                        help="used with --emit-tile-json: mark the tile as blocking")
    parser.add_argument("--no-contact-sheet", action="store_true",
                        help="skip the side-by-side comparison image")
    parser.add_argument("--list-models", action="store_true",
                        help="show what would be used for --model and --lora, then exit")
    parser.add_argument("--dry-run", action="store_true",
                        help="skip the model entirely and run the image pipeline on "
                             "synthetic input; verifies paths and output without a GPU")
    return parser.parse_args()


def slugify(text: str) -> str:
    """Match SpritePaths.cpp exactly: runs of non-alphanumerics become one
    underscore, so "mossy stone" and "mossy  stone" name the same file."""
    out = []
    pending = False
    for ch in text:
        if ch.isalnum():
            if pending and out:
                out.append("_")
            pending = False
            out.append(ch.lower())
        else:
            pending = True
    return "".join(out)


def candidate_comfy_roots(explicit: str) -> list[Path]:
    """Where a ComfyUI data directory might be. This project's own setup mounts
    ~/comfyui-data at /data inside a container, so both spellings are tried: a
    script run in the container sees /data, one run on the host sees ~/comfyui-data.

    Duplicates are collapsed so --list-models does not report the same directory
    twice, which matters because on this machine ~/comfyui-data IS /data when
    inside the container.
    """
    import os

    roots = []
    if explicit:
        roots.append(Path(explicit).expanduser())
    env = os.environ.get("COMFY_DATA", "")
    if env:
        roots.append(Path(env).expanduser())
    roots.append(Path("/data"))
    roots.append(Path.home() / "comfyui-data")
    roots.append(Path.home() / "ComfyUI")

    seen = set()
    unique = []
    for root in roots:
        try:
            key = root.resolve()
        except OSError:
            key = root
        if key in seen or not root.is_dir():
            continue
        seen.add(key)
        unique.append(root)
    return unique


def find_model(explicit: str, roots: list[Path], subdir: str, prefer: str = "") -> str:
    """The .safetensors to use, or "".

    `prefer` is a case-insensitive substring that wins if several candidates are
    present. For the LoRA that is used to pick the pixel-art one over whatever
    else happens to be in models/loras/, because choosing the wrong LoRA silently
    produces non-pixel-art output and looks like the pipeline is broken.
    """
    if explicit:
        return explicit

    candidates: list[Path] = []
    for root in roots:
        directory = root / "models" / subdir
        if directory.is_dir():
            candidates.extend(sorted(directory.glob("*.safetensors")))
            candidates.extend(sorted(directory.glob("*.ckpt")))

    if not candidates:
        return ""

    if prefer:
        preferred = [c for c in candidates if prefer.lower() in c.name.lower()]
        if preferred:
            return str(preferred[0])
    return str(candidates[0])


def load_shared_palette(path: str):
    """The pack's palette as a list of RGB tuples, or None when there is none.

    Imported inside the function because tools/palette.py needs Pillow at import
    time, and this script deliberately keeps --dry-run and --list-models working
    without Pillow installed.
    """
    if not path or not Path(path).exists():
        return None
    # Check Pillow before importing tools/palette.py, which prints its own
    # "needs Pillow" banner and exits when it is missing. This script keeps
    # --list-models and --dry-run usable without Pillow, so that would be noise
    # for a case that is not an error here.
    try:
        import PIL  # noqa: F401
    except ImportError:
        print(f"note: cannot read {path} without Pillow; quantising per image")
        return None

    import palette as palette_tool

    return palette_tool.load_palette(str(path))


def snap_to_palette(image, palette, intermediate: int = 256):
    """Every pixel to its nearest palette colour, perceptually, memoised.

    Two stages on purpose. The direct route is O(distinct colours x palette size)
    of OKLab distance in Python, and a 512x512 model render carries on the order of
    100k distinct colours -- tens of seconds per tile. Reducing to a few hundred
    first makes the search trivial and changes nothing visible, because the result
    is downscaled to 32px immediately afterwards anyway.

    Uses tools/palette.py's conversion and search so both backends snap by exactly
    the same rule; a second implementation here would drift from it.
    """
    import palette as palette_tool
    from PIL import Image

    reduced = image.quantize(colors=intermediate, method=Image.MEDIANCUT,
                             dither=Image.NONE).convert("RGB")
    lab = [palette_tool.rgb_to_oklab(c) for c in palette]
    cache: dict = {}
    out = [palette[palette_tool.nearest_index((r, g, b), palette, lab, cache)]
           for r, g, b in reduced.getdata()]
    result = Image.new("RGB", reduced.size)
    result.putdata(out)
    return result


def pixelize(image, tile_size: int, colors: int, palette=None):
    """Quantise then downscale. Order matters: quantising after downscaling would
    be averaging colours the palette then has to approximate, which is how you get
    mud. NEAREST is not a detail -- BILINEAR here is what makes generated pixel
    art look like a photo of pixel art.

    `palette` is the pack's shared palette, from `tools/palette.py derive`. When
    given it replaces the per-image median cut, so every tile lands on the same set
    of colours instead of on a palette of its own -- which is the difference
    between a set of tiles and a set of tiles that match. See
    docs/asset-generation-design.md 7.3.
    """
    from PIL import Image

    image = image.convert("RGB")
    if palette:
        quantised = snap_to_palette(image, palette)
    else:
        quantised = image.quantize(colors=colors, method=Image.MEDIANCUT,
                                   dither=Image.NONE).convert("RGB")
    return quantised.resize((tile_size, tile_size), Image.NEAREST)


def write_tile_json(name: str, material: str, blocks: bool) -> Path:
    """A catalog entry so the tile can be chosen in the map editor. The convention
    path already resolves the sprite; this is only what makes the tile selectable
    as a *kind* of ground."""
    import json

    colors = [110, 200, 110, 255]  # placeholder, the sprite supplies the look
    payload = {
        "name": material,
        "character": "?",
        "characterColor": colors,
        "backgroundColor": [0, 0, 0, 0],
        "blocksTile": bool(blocks),
        "sprite": f"data/sprites/tile_{name}.png",
    }
    path = TILES_DIR / f"{name}.json"
    path.write_text(json.dumps(payload, indent=2) + "\n")
    return path


def build_contact_sheet(generated, out_path: Path, tile_size: int) -> bool:
    """Generated tiles on the first row, existing game art on the second, both at
    true game scale with a hard zoom below. The only question that matters is
    whether they belong together, and that cannot be judged from the 512px render.
    """
    from PIL import Image

    references = []
    for stem in REFERENCE_TILES:
        candidate = SPRITES_DIR / f"{stem}.png"
        if candidate.exists():
            references.append((stem, Image.open(candidate).convert("RGB")))

    if not references:
        print("  (no reference sprites found; contact sheet will show only the new tiles)")

    scale = 4  # a 32px tile is too small to judge on screen at 1x
    pad = 6
    cols = max(len(generated), len(references))
    cell = tile_size * scale
    width = cols * cell + (cols + 1) * pad
    height = 2 * cell + 3 * pad

    sheet = Image.new("RGB", (width, height), (24, 24, 28))

    def paste(image, col, row):
        x = pad + col * (cell + pad)
        y = pad + row * (cell + pad)
        sheet.paste(image.resize((cell, cell), Image.NEAREST), (x, y))

    for i, image in enumerate(generated):
        paste(image, i, 0)
    for i, (_, image) in enumerate(references):
        paste(image.resize((tile_size, tile_size), Image.NEAREST), i, 1)

    sheet.save(out_path)
    print(f"  contact sheet: {out_path}")
    print(f"    top row    = generated {out_path.stem.replace('_sheet', '')}")
    if references:
        print(f"    bottom row = {' , '.join(name for name, _ in references)}")
    return True


def synthetic_source(size: int, index: int):
    """A deterministic stand-in for a render, so --dry-run exercises quantisation,
    downscaling, naming and the contact sheet without torch installed."""
    from PIL import Image, ImageDraw

    image = Image.new("RGB", (size, size), (30 + index * 7, 90, 40 + index * 5))
    draw = ImageDraw.Draw(image)
    for y in range(0, size, 4):
        draw.line([(0, y), (size, y)], fill=(60, 140 + (y % 40), 70), width=1)
    for x in range(0, size, 16):
        draw.rectangle([x, 0, x + 7, size], fill=(90, 170, 90))
    return image


def load_pipeline(args):
    """Returns (pipe, description). Imported lazily so --dry-run works without a
    multi-gigabyte install."""
    import torch
    from diffusers import DPMSolverMultistepScheduler, StableDiffusionPipeline

    if args.model.endswith(".safetensors") or Path(args.model).is_file():
        pipe = StableDiffusionPipeline.from_single_file(
            args.model, torch_dtype=torch.float16,
            safety_checker=None, requires_safety_checker=False,
        )
    else:
        pipe = StableDiffusionPipeline.from_pretrained(
            args.model, torch_dtype=torch.float16,
            safety_checker=None, requires_safety_checker=False,
        )

    pipe = pipe.to("cuda")
    # 8 GB cards OOM on a LoRA plus 512px batches without this.
    pipe.enable_attention_slicing()
    pipe.scheduler = DPMSolverMultistepScheduler.from_config(pipe.scheduler.config)

    description = f"{args.model} on {torch.cuda.get_device_name(0)}"
    if args.lora:
        pipe.load_lora_weights(args.lora, adapter_name="pixel")
        pipe.set_adapters(["pixel"], adapter_weights=[args.lora_scale])
        description += f" + LoRA {Path(args.lora).name} @ {args.lora_scale}"
    return pipe, description


def main() -> int:
    args = parse_args()

    roots = candidate_comfy_roots(args.comfy_data)
    args.model = find_model(args.model, roots, "checkpoints")
    # "pixel" picks the pixel-art LoRA over any other LoRA present. An empty
    # result is supported: the run then uses the base model alone, which is worth
    # seeing at least once so the LoRA's contribution is obvious.
    args.lora = find_model(args.lora, roots, "loras", prefer="pixel")

    if args.list_models:
        print("ComfyUI data directories searched:")
        for root in roots:
            print(f"  {root}")
        if not roots:
            print("  (none found -- pass --comfy-data <dir>)")
        print()
        print(f"checkpoint : {args.model or '(none found)'}")
        print(f"lora       : {args.lora or '(none found -- will run without a LoRA)'}")
        if args.lora:
            print(f"             trigger words still have to come from its model page;")
            print(f"             pass them with --trigger. Current: {args.trigger!r}")
        return 0

    if not args.material:
        print("error: a material is required (or use --list-models)", file=sys.stderr)
        return 2

    if not args.dry_run and not args.model:
        print("error: no SD 1.5 checkpoint found. Pass --model, or --comfy-data "
              "pointing at a ComfyUI data directory containing "
              "models/checkpoints/*.safetensors.", file=sys.stderr)
        return 2

    name = args.name or slugify(args.material)
    if not name:
        print("error: could not derive an output name from the material", file=sys.stderr)
        return 2

    if not SPRITES_DIR.is_dir():
        print(f"error: {SPRITES_DIR} does not exist -- run this from the repo", file=sys.stderr)
        return 2

    out_path = SPRITES_DIR / f"tile_{name}.png"
    raw_dir = Path(args.raw_dir)
    raw_dir.mkdir(parents=True, exist_ok=True)

    print(f"material   : {args.material}")
    print(f"output     : {out_path}")
    print(f"tile size  : {args.tile_size}px, {args.colors} colours")
    shared_palette = load_shared_palette(args.palette)
    if shared_palette:
        print(f"palette    : {len(shared_palette)} colours from {args.palette}")
    else:
        print(f"palette    : none at {args.palette} -- quantising each image to "
              f"its own {args.colors} colours")
        print(f"             (python3 tools/palette.py derive --sprites "
              f"data/sprites --colors 128)")
    print(f"count      : {args.count}, seed {args.seed}")
    print(f"checkpoint : {args.model}")
    print(f"lora       : {args.lora or '(none)'}")
    print()

    if args.dry_run:
        print("dry run: generating synthetic sources, no model will be loaded")
        sources = [synthetic_source(512, i) for i in range(args.count)]
    else:
        try:
            pipe, description = load_pipeline(args)
        except ImportError as exc:
            print(f"error: {exc}\n"
                  f"Install torch/diffusers first -- see tools/README.md.", file=sys.stderr)
            return 3
        except Exception as exc:  # noqa: BLE001 - surface any load failure plainly
            print(f"error: could not load the model: {exc}", file=sys.stderr)
            print("If CUDA is the problem, check that torch.cuda.is_available() is True.",
                  file=sys.stderr)
            return 3

        import torch

        print(f"model      : {description}")
        prompt = PROMPT_TEMPLATE.format(trigger=args.trigger, material=args.material)
        print(f"prompt     : {prompt}")
        print()
        print("generating... (the first run may download several GB of weights)")

        sources = pipe(
            prompt,
            negative_prompt=NEGATIVE_PROMPT,
            num_inference_steps=args.steps,
            guidance_scale=args.cfg,
            width=512,
            height=512,
            num_images_per_prompt=args.count,
            generator=torch.Generator(device="cuda").manual_seed(args.seed),
        ).images

    # The raw renders are kept because "is it the model or the downscale?" is the
    # first question when a result looks wrong.
    pixelized = []
    for i, source in enumerate(sources):
        source.save(raw_dir / f"{name}_{i}.png")
        tile = pixelize(source, args.tile_size, args.colors, shared_palette)
        tile.save(SPRITES_DIR / f"tile_{name}_{i}.png")
        pixelized.append(tile)

    # The first result is also written under the plain convention name, so a tile
    # JSON (or an existing catalog entry) picks it up without renaming.
    pixelized[0].save(out_path)

    print(f"wrote {len(pixelized)} tiles to {SPRITES_DIR}/tile_{name}_*.png")
    print(f"       and {out_path.name} for the first one")
    print(f"raw 512px renders kept in {raw_dir}")

    if args.emit_tile_json:
        path = write_tile_json(name, args.material, args.blocks_tile)
        print(f"wrote tile definition {path}")

    if not args.no_contact_sheet:
        print()
        build_contact_sheet(pixelized, SPRITES_DIR / f"tile_{name}_sheet.png",
                            args.tile_size)

    print()
    print("Now look at it in the game, not in an image viewer:")
    print(f"  1. name it in a map (tileInfo name: \"{args.material}\") or run with "
          f"--emit-tile-json")
    print(f"  2. paint a 10x10 patch in the map editor so seams show")
    print(f"  3. compare against {', '.join(REFERENCE_TILES)} in the same field")
    return 0


if __name__ == "__main__":
    sys.exit(main())
