#!/usr/bin/env python3
"""Generate AIRogue prop sprites with the local ComfyUI instance.

Scratch tooling; not part of the game build and not referenced by CMakeLists.txt.

Why ComfyUI and not the OpenRouter path (`gen_object_sheet.py`)
--------------------------------------------------------------
The Gemini path bills per request, so it only makes sense as one big sheet. Local
diffusion costs nothing per image, which changes the shape of the tool: one
request per prop is affordable, and that matters because a batch sampler makes it
trivially cheap to reroll just the prop that came out wrong.

The pipeline is the owner's `tools/workflows/pixelart_sd15.json` template
(SD 1.5 + PixelArtRedmond15V LoRA + the ComfyUI-PixelArt-Detector post-process),
rebuilt as an API prompt rather than driving the saved UI graph. The UI graph is
a 4x-bypassed experiment with a 3-sampler refiner chain and a VAELoader in
"never" mode; the API prompt below is the same model path expressed as one
straight line, which is what makes it scriptable.

Output discipline
-----------------
Writes to `sprite_preview/comfy_out/` only. It never touches `data/sprites/`,
because a generated prop is not automatically the art pack (`docs/
asset-generation-design.md` §1.2: the generated layer never feeds back into the
thing it emulates). Promotion into the game is a deliberate, separate copy.

Usage
-----
    # free: build every prompt, print them, show the graph, touch nothing
    python3 sprite_preview/comfy_props.py --dry-run barrel well cart

    # the real thing -- one 512px render per prop, 2 variants each
    python3 sprite_preview/comfy_props.py barrel well cart

    # reroll one prop with different seeds, no prompt rebuild
    python3 sprite_preview/comfy_props.py barrel --count 4

Reads nothing secret. Talks only to 127.0.0.1:8188.
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

COMFY = os.environ.get("COMFY_URL", "http://127.0.0.1:8188")
# Default output location. Every experiment should pass --out-dir and get its
# own directory, because raws are namespaced under it and a shared location means
# one experiment silently overwrites another's raw renders.
OUT_DIR = os.path.join(HERE, "comfy_out")

# The two artefacts installed in the owner's ComfyUI. Confirmed against
# /object_info at runtime -- see verify_models().
CHECKPOINT = "v1-5-pruned-emaonly-fp16.safetensors"
LORA = "PixelArtRedmond15V-PixelArt-PIXARFK.safetensors"

# LoRA trigger words. Omitted trigger words are the single most common reason a
# LoRA appears to do nothing (tools/README.md §2), so they are hardcoded into
# the style block rather than left to the caller.
LORA_TRIGGER = "pixel art, PixelArtRedmond, PIXARFK"

# Activation words for a dedicated pixel-art checkpoint, used when --no-lora is
# given and no --trigger overrides it. AziibPixelMix's model page names these
# exactly; using the wrong trigger words is the documented reason a model appears
# to do nothing.
PIXEL_ART_TRIGGER = "pixel art, pixel world"

# Same design intent as gen_object_sheet.py's STYLE block: one flat magenta
# background, no frame, no text, exactly one object, centred. SD 1.5 responds to
# the tag-soup style of prompting, so this is comma-separated rather than prose.
#
# Two lessons are baked into these variants, both learned from the first live run:
#
#  1. Asking for "chunky readable pixels" makes the model pixelate the whole
#     512px canvas into ~64px blocks -- it renders a *picture of* pixel art rather
#     than a sprite. The first barrel came back as 16 huge blocks across. The
#     pixel look has to come from the LoRA and from the 512->32 downscale, never
#     from the prompt.
#  2. A flat magenta background is a large, easy-to-draw, high-contrast region;
#     asked for alongside a complex object, SD 1.5 drops it and paints whatever
#     else is in the prompt. It came back green. `MASK_STYLE` asks for the thing
#     the model actually wants to draw (a plain solid colour) and leaves the
#     colour choice to be *detected* rather than specified -- see comfy_sheet.py,
#     which keys out whatever flat colour actually surrounds the subject.
STYLE = (
    "{trigger}, a single {subject}, centred, full object in frame, "
    "game item sprite, 16-bit, chunky readable pixels, limited palette, hard "
    "edges, black outline, soft top-down lighting, plain flat magenta "
    "background, no shadow on the ground"
)

# The style that the probes on this machine actually favoured. "sprite" and
# "game asset" weight the LoRA toward a single object at sprite scale; the
# explicit "fills the frame with a margin" fights the LoRA's tendency to crop
# tight, which is what cut the first barrel in half.
STYLE_V2 = (
    "{trigger}, {subject}, game item sprite, single object centred, "
    "the whole object visible with a small margin around it, empty flat "
    "background, soft lighting from the upper left"
)

STYLE_V2_NEG = (
    "cropped, cut off, zoomed in, out of frame, close-up, scenery, landscape, "
    "sky, ground, grass, texture background, pattern background, tiled, "
    "seamless, multiple objects, two objects, reflection, shadow, drop shadow, "
    "contact shadow, cast shadow, ground shadow, glow, vignette, halo, "
    "floor, surface, table, stand, plinth, pedestal"
)

# Negative prompt. "realistic/photorealistic/3d" comes from the template's own
# default; the rest suppress the specific failure modes seen when a single-object
# prompt meets an SD 1.5 checkpoint that wants to draw a scene.
NEGATIVE = (
    "realistic, photorealistic, 3d, render, photograph, blurry, smooth gradient, "
    "anti-aliasing, jpeg artifacts, text, letters, numbers, watermark, signature, "
    "frame, border, grid, multiple objects, duplicate, scenery, landscape, sky, "
    "ground plane, cast shadow, person, human, hands"
)

# A checkpoint-specific template. AziibPixelMix is a *scene* model: asked for "a
# wooden cart" it paints a cart in a street with a fence and a tree, and the whole
# frame fills with scenery. Everything below is aimed at one thing -- forcing a
# single object onto an empty field -- because the style is already correct and
# only the composition is wrong. The word "object" is load-bearing: it pulls the
# model away from "illustration" toward "asset".
STYLE_V3 = (
    "{trigger}, a single {subject}, one object only, isolated object, "
    "no background scene, empty plain flat background, item icon, "
    "centred, whole object visible, margin around the object"
)

STYLE_V3_NEG = (
    "scene, scenery, landscape, landscape background, building, house, "
    "architecture, room, interior, street, path, road, ground, dirt, grass, "
    "moss, plants, flowers, vines, tree, forest, leaves, sky, clouds, water, "
    "pond, fence, wall, multiple objects, clutter, props, collection, "
    "diorama, isometric scene, decorated, ornamented, frame, border, "
    "censored, mosaic, tiles, grid, pixelated text, watermark, signature"
)

STYLES = {
    "v1": (STYLE, NEGATIVE),
    "v2": (STYLE_V2, NEGATIVE + ", " + STYLE_V2_NEG),
    "v3": (STYLE_V3, NEGATIVE + ", " + STYLE_V2_NEG + ", " + STYLE_V3_NEG),
}

# The prop vocabulary. Each entry is both the output filename stem and the
# subject phrase. Kept as explicit art direction rather than a bare noun because
# a bare noun is what makes SD 1.5 draw a scene instead of an icon: "barrel" can
# come back as a tavern interior, "a wooden barrel standing upright, iron bands"
# cannot.
PROPS = {
    "barrel": "wooden barrel standing upright, curved iron bands around it, "
              "oak brown staves, viewed slightly from above",
    "well": "stone wishing well, round grey masonry rim, two upright posts and "
            "a small peaked wooden roof over it, a rope and bucket",
    "cart": "wooden hand cart, two large spoked wheels, flat plank bed, two "
            "handles pointing forward",
    "crate": "wooden shipping crate, square, plank sides with darker diagonal "
             "brace boards, seen at a slight angle",
    "signpost": "wooden signpost, a single post with two arrow-shaped boards "
                "pointing in different directions",
    "lamp_post": "iron lamp post, tall thin dark metal pole, a small glass "
                 "lantern box with a warm yellow glow at the top",
    "barrel_stack": "two wooden barrels, one standing and one lying on its side "
                    "beside it, iron bands",
    "wheelbarrow": "wooden wheelbarrow, single small front wheel, deep plank "
                   "tray, two handles with legs at the back",
    "wood_pile": "a stack of chopped firewood logs, round cut ends facing the "
                 "viewer, arranged in a rough pyramid",
    "hay_bale": "a rectangular bale of golden hay, bound with two strands of "
                "twine, dry straw texture",
    "bucket": "wooden water bucket with a metal handle, iron hoops, seen from a "
              "three-quarter angle",
    "fence_gate": "wooden farm gate, horizontal planks between two posts, a "
                  "diagonal brace, seen from the front",
    "market_stall": "wooden market stall, plank counter with four posts and a "
                    "striped canvas awning on top",
    "ladder": "wooden ladder leaning, two long side rails with evenly spaced "
              "rungs, seen from the front",
    "water_trough": "hollowed wooden water trough, rectangular, filled with "
                    "blue water, dark wood sides",
    "anvil": "blacksmith anvil on a wooden stump, heavy dark iron, pointed horn "
             "on one side, flat face on top",
    "cauldron": "round black iron cauldron on three short legs, wide rim, dark "
                "soot-stained metal, seen from a slight angle",
    "sack": "tied burlap sack, bulging at the bottom, gathered and knotted at "
            "the top, coarse tan cloth",
    "flower_pot": "terracotta flower pot with a small green plant and one red "
                  "flower growing out of it",
    "bird_house": "small wooden bird house on a post, peaked roof, round dark "
                  "entrance hole, tiny perch below the hole",
    "stone_bench": "grey stone bench, thick rectangular slab seat on two solid "
                   "stone block legs",
    "fireplace": "stone fireplace, tall arched hearth opening with burning logs "
                 "and orange flames inside, grey masonry surround, a wooden "
                 "mantel shelf on top",
    "firewood_axe": "woodcutting axe stuck upright in a chopping block, wooden "
                    "handle, steel head",
    "lantern": "hanging iron lantern, four glass panes, warm yellow flame "
               "inside, ring handle on top",
    "church_bell": "large bronze church bell, wide flared mouth, small loop "
                   "handle on top, weathered green-brown patina",
    "wheat_sack_cart": "small wooden cart loaded with two tied sacks of grain, "
                       "side rails, wooden wheels",
}


# --------------------------------------------------------------------------
# ComfyUI plumbing
# --------------------------------------------------------------------------

def api(path):
    return COMFY + path


def get_json(path, timeout=30):
    with urllib.request.urlopen(api(path), timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def post_json(path, payload, timeout=60):
    data = json.dumps(payload).encode()
    req = urllib.request.Request(
        api(path), data=data, headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def verify_models(checkpoint=CHECKPOINT, lora=None):
    """Fail loudly and early if the checkpoint or LoRA is not installed.

    A missing model is otherwise reported by ComfyUI as a validation error deep
    in a queue response, which is a much worse error message than this one.
    """
    try:
        stats = get_json("/system_stats")
    except (urllib.error.URLError, OSError) as exc:
        sys.exit(
            f"cannot reach ComfyUI at {COMFY} ({exc}).\n"
            "Start it, or set COMFY_URL to where it listens."
        )

    info = get_json("/object_info", timeout=120)
    ckpts = info["CheckpointLoaderSimple"]["input"]["required"]["ckpt_name"][0]
    loras = info["LoraLoader"]["input"]["required"]["lora_name"][0]

    problems = []
    if checkpoint not in ckpts:
        problems.append(f"checkpoint {checkpoint!r} not installed; have {ckpts}")
    # A dedicated pixel-art checkpoint already contains the style, so a LoRA is
    # optional -- and stacking a style LoRA on a style checkpoint usually fights
    # it. `lora=None` is the documented way to say "checkpoint only".
    if lora and lora not in loras:
        problems.append(f"LoRA {lora!r} not installed; have {loras}")
    if problems:
        sys.exit("ComfyUI is missing required models:\n  " + "\n  ".join(problems))

    dev = stats.get("devices", [{}])[0]
    return dev.get("name", "unknown device"), stats["system"]["comfyui_version"]


def build_prompt(subject, seed, count_negative=True, steps=28, cfg=7.0,
                 width=512, height=512, lora_scale=0.85, style="v2",
                 sampler="euler", scheduler="normal", init_image=None,
                 denoise=1.0, checkpoint=CHECKPOINT, lora=LORA,
                 trigger=LORA_TRIGGER):
    """Build one API-format graph: ckpt -> lora -> clip x2 -> ksampler -> decode.

    `batch_size` carries the variant count, so N variants cost one queue slot and
    one model load instead of N. Seeds are spread deterministically from `seed`
    so a reroll is reproducible from a single number.
    """
    template, neg_text = STYLES[style]
    # The trigger string is the model's own activation words. For the base
    # checkpoint that is the LoRA's trigger; for a dedicated pixel-art
    # checkpoint it is that checkpoint's trained words, which are different and
    # are passed in rather than hardcoded.
    positive = template.format(trigger=trigger, subject=subject)
    negative = neg_text if count_negative else ""

    graph = {
        "1": {
            "class_type": "CheckpointLoaderSimple",
            "inputs": {"ckpt_name": checkpoint},
        },
        "3": {
            "class_type": "CLIPTextEncode",
            "inputs": {"clip": ["1", 1], "text": positive},
        },
        "4": {
            "class_type": "CLIPTextEncode",
            "inputs": {"clip": ["1", 1], "text": negative},
        },
        "5": {
            "class_type": "EmptyLatentImage",
            "inputs": {"width": width, "height": height, "batch_size": 1},
        },
        "6": {
            "class_type": "KSampler",
            "inputs": {
                "model": ["1", 0],
                "positive": ["3", 0],
                "negative": ["4", 0],
                "latent_image": ["5", 0],
                "seed": seed,
                "steps": steps,
                "cfg": cfg,
                "sampler_name": sampler,
                "scheduler": scheduler,
                "denoise": denoise,
            },
        },
        "7": {
            "class_type": "VAEDecode",
            "inputs": {"samples": ["6", 0], "vae": ["1", 2]},
        },
        # SaveImage writes into ComfyUI's own output/ directory. That is the
        # honest handoff: the pixels are fetched back over /history and written
        # where this tool wants them, and ComfyUI keeps its copy as the raw
        # render ("keep the raw renders, always" -- tools/README.md §3).
        "8": {
            "class_type": "SaveImage",
            "inputs": {"images": ["7", 0], "filename_prefix": "air_props"},
        },
    }

    if lora:
        # Inserted rather than conditionally building the whole graph: the LoRA
        # is a pure model+clip patch, so adding it only re-points the three
        # consumers at node 2 instead of node 1.
        graph["2"] = {
            "class_type": "LoraLoader",
            "inputs": {
                "model": ["1", 0],
                "clip": ["1", 1],
                "lora_name": lora,
                "strength_model": lora_scale,
                "strength_clip": lora_scale,
            },
        }
        graph["3"]["inputs"]["clip"] = ["2", 1]
        graph["4"]["inputs"]["clip"] = ["2", 1]
        graph["6"]["inputs"]["model"] = ["2", 0]

    if init_image:
        # img2img: load the reference sprite, upscale it to the generation size
        # with NEAREST (so its pixel structure is not smeared), encode it, and
        # feed that as the sampler's latent. `denoise` then decides how far the
        # result may drift from the reference.
        #
        # Why this is the strongest consistency lever available here: the LoRA
        # alone fixes the *style* but not the composition, which is why a batch
        # of eleven props came back with eleven different camera angles and
        # outline weights. Seeding every render from the same reference image
        # gives the model a concrete answer to "what does a sprite look like in
        # this game" instead of a verbal one.
        graph["9"] = {
            "class_type": "LoadImage",
            "inputs": {"image": init_image},
        }
        graph["10"] = {
            "class_type": "ImageScale",
            "inputs": {
                "image": ["9", 0],
                "upscale_method": "nearest-exact",
                "width": width,
                "height": height,
                "crop": "disabled",
            },
        }
        graph["11"] = {
            "class_type": "VAEEncode",
            "inputs": {"pixels": ["10", 0], "vae": ["1", 2]},
        }
        graph["6"]["inputs"]["latent_image"] = ["11", 0]
        # The reference is 32x32 scaled up: beyond ~0.75 denoise the model
        # abandons it entirely and this is just text2img with extra steps.
        graph["6"]["inputs"]["denoise"] = denoise

    return graph


def queue(prompt, client_id):
    return post_json("/prompt", {"prompt": prompt, "client_id": client_id})


def wait_for(prompt_id, client_id, timeout=600, poll=1.0):
    """Poll /history until the prompt appears, then return its outputs.

    /history keeps a finished job forever, so the termination condition is
    presence in history rather than a job-state transition. An execution error
    is surfaced with ComfyUI's own node id and message, which is the only
    actionable part of a failed diffusion run.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        hist = get_json(f"/history/{prompt_id}")
        entry = hist.get(prompt_id)
        if entry:
            status = entry.get("status", {})
            if status.get("status_str") == "error":
                for kind, payload in status.get("messages", []):
                    if kind == "execution_error":
                        sys.exit(
                            f"ComfyUI node {payload.get('node_id')} "
                            f"({payload.get('node_type')}) failed: "
                            f"{payload.get('exception_message')}"
                        )
                sys.exit(f"ComfyUI reported an error for {prompt_id}")
            return entry.get("outputs", {})
        time.sleep(poll)
    sys.exit(f"timed out after {timeout}s waiting for {prompt_id}")


def upload_image(path, name):
    """Upload a reference sprite into ComfyUI's input/ directory.

    ComfyUI's LoadImage node reads only from its own input folder, so a reference
    living in data/sprites has to be copied in over the HTTP API. Returns the
    filename to hand to LoadImage (ComfyUI may rename on collision).
    """
    with open(path, "rb") as handle:
        blob = handle.read()

    boundary = "----air" + uuid.uuid4().hex
    body = b""
    body += ("--" + boundary + "\r\n").encode()
    body += (f'Content-Disposition: form-data; name="image"; filename="{name}"'
             "\r\n").encode()
    body += b"Content-Type: image/png\r\n\r\n"
    body += blob
    body += ("\r\n--" + boundary + "--\r\n").encode()

    req = urllib.request.Request(
        api("/upload/image"), data=body,
        headers={"Content-Type": "multipart/form-data; boundary=" + boundary},
    )
    with urllib.request.urlopen(req, timeout=120) as resp:
        result = json.loads(resp.read().decode())
    # The response carries the stored name and any subfolder it landed in;
    # LoadImage wants the relative path in that case.
    name = result.get("name", name)
    sub = result.get("subfolder") or ""
    return f"{sub}/{name}" if sub else name


def fetch_image(filename, subfolder, folder_type):
    query = urllib.parse.urlencode(
        {"filename": filename, "subfolder": subfolder, "type": folder_type}
    )
    with urllib.request.urlopen(api("/view?" + query), timeout=120) as resp:
        return resp.read()


# --------------------------------------------------------------------------
# Prompt construction
# --------------------------------------------------------------------------

def resolve(name):
    """Map a CLI name to its subject phrase, failing with the real vocabulary."""
    if name in PROPS:
        return PROPS[name]
    # Allow a free-form phrase: `comfy_props.py "a stone oven"`.
    if " " in name:
        return name
    sys.exit(
        f"unknown prop {name!r}. Known props:\n  "
        + "  ".join(sorted(PROPS))
        + "\n(Or pass a multi-word phrase to describe one yourself.)"
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("props", nargs="*", help="prop names, or a free-form phrase")
    ap.add_argument("--dry-run", action="store_true",
                    help="print prompts and the graph, contact nothing")
    ap.add_argument("--count", type=int, default=2,
                    help="variants per prop (default 2)")
    ap.add_argument("--seed", type=int, default=None,
                    help="base seed; default is derived from the clock")
    ap.add_argument("--steps", type=int, default=28)
    ap.add_argument("--cfg", type=float, default=7.0)
    ap.add_argument("--lora-scale", type=float, default=0.85)
    ap.add_argument("--size", type=int, default=256,
                    help="generation canvas. 256 is the default because the "
                         "model's pixel-art grid is ~half the canvas, so 256 "
                         "yields a 128px grid and 32px output stays crisp; at "
                         "512 the 8:1 downscale turns to mush (README 3.6)")
    ap.add_argument("--style", choices=sorted(STYLES), default="v2",
                    help="prompt template: v1 is the original, v2 the fixed one")
    ap.add_argument("--sampler", default="euler")
    ap.add_argument("--scheduler", default="normal")
    ap.add_argument("--init-image", default=None,
                    help="reference sprite for img2img (path in data/sprites/)")
    ap.add_argument("--denoise", type=float, default=0.65,
                    help="img2img strength: lower stays closer to --init-image")
    ap.add_argument("--out-dir", default=OUT_DIR,
                    help="where finished renders are copied (raw always in raw512/)")
    ap.add_argument("--checkpoint", default=CHECKPOINT,
                    help="checkpoint filename installed in ComfyUI")
    ap.add_argument("--no-lora", action="store_true",
                    help="skip the LoRA -- correct for a dedicated pixel-art "
                         "checkpoint, which already contains the style")
    ap.add_argument("--trigger", default=None,
                    help="activation words for the chosen model "
                         "(default: the LoRA's trigger)")
    ap.add_argument("--list", action="store_true", help="list prop vocabulary")
    args = ap.parse_args()

    if args.list:
        for name in sorted(PROPS):
            print(f"{name:20s} {PROPS[name]}")
        return

    names = args.props
    if not names:
        ap.error("name at least one prop (or --list)")

    # `--no-lora` on the default checkpoint is the one combination that is almost
    # never intended: it strips the LoRA *and* leaves a general-purpose
    # photographic base model, so the output has no pixel-art style at all. Catch
    # it rather than silently producing a batch of non-pixel-art.
    if args.no_lora and args.checkpoint == CHECKPOINT:
        sys.exit(
            f"--no-lora with the default checkpoint ({CHECKPOINT}) drops the "
            "pixel-art LoRA and leaves a photographic base model.\n"
            "Pass the pixel-art checkpoint instead:\n"
            "  --no-lora --checkpoint aziibpixelmix_v10.safetensors"
        )

    base_seed = args.seed if args.seed is not None else int(time.time()) % 2**31
    lora_name = None if args.no_lora else LORA
    trigger = args.trigger if args.trigger is not None else (
        LORA_TRIGGER if lora_name else PIXEL_ART_TRIGGER)

    if args.dry_run:
        print(f"ComfyUI would be asked at {COMFY}")
        print(f"checkpoint : {args.checkpoint}")
        print(f"LoRA       : {lora_name or '(none -- checkpoint only)'}"
              + (f" @ {args.lora_scale}" if lora_name else ""))
        print(f"trigger    : {trigger}")
        print(f"size/steps : {args.size}x{args.size} / {args.steps} steps "
              f"cfg {args.cfg}")
        print(f"variants   : {args.count} per prop, base seed {base_seed}")
        print(f"output     : {args.out_dir}/<prop>_<n>.png "
              f"(and raw renders in {args.out_dir}/raw/)")
        print()
        for name in names:
            subject = resolve(name)
            prompt = build_prompt(subject, base_seed, steps=args.steps,
                                  cfg=args.cfg, width=args.size,
                                  height=args.size,
                                  lora_scale=args.lora_scale,
                                  style=args.style, sampler=args.sampler,
                                  scheduler=args.scheduler,
                                  init_image="<ref>" if args.init_image else None,
                                  denoise=args.denoise,
                                  checkpoint=args.checkpoint, lora=lora_name,
                                  trigger=trigger)
            print(f"--- {name} ---")
            print(prompt["3"]["inputs"]["text"])
            print(f"    negative: {prompt['4']['inputs']['text']}")
            print()
        print("graph (barrel, the first prop):")
        graph = build_prompt(resolve(names[0]), base_seed, steps=args.steps,
                            cfg=args.cfg, width=args.size, height=args.size,
                            lora_scale=args.lora_scale, style=args.style,
                            sampler=args.sampler, scheduler=args.scheduler,
                            checkpoint=args.checkpoint, lora=lora_name,
                            trigger=trigger)
        print(json.dumps(graph, indent=2)[:1800])
        return

    device, version = verify_models(args.checkpoint, lora_name)
    print(f"ComfyUI {version} on {device}")

    os.makedirs(args.out_dir, exist_ok=True)
    # Raws live beside the finals they produced. Hardcoding one shared raw dir
    # meant a second experiment at a different size silently overwrote the first
    # one's raw renders -- and raws are the expensive artefact, so losing them is
    # the worst possible failure (tools/README.md §11.2).
    raw_dir = os.path.join(args.out_dir, "raw")
    os.makedirs(raw_dir, exist_ok=True)

    client_id = str(uuid.uuid4())
    manifest = []

    # Upload the reference once, not per prop: it is the same file every time and
    # a re-upload per render just adds latency.
    init_name = None
    if args.init_image:
        ref = args.init_image
        if not os.path.isabs(ref) and not os.path.exists(ref):
            ref = os.path.join(ROOT, "data", "sprites", ref)
        if not os.path.exists(ref):
            sys.exit(f"no such reference image: {args.init_image}")
        init_name = upload_image(ref, os.path.basename(ref))
        print(f"reference {os.path.basename(ref)} -> ComfyUI as {init_name} "
              f"(denoise {args.denoise})")

    for name in names:
        subject = resolve(name)
        # Deterministic per-prop seed offset so rerolling one prop does not
        # silently reroll the others. crc32, not hash(): hash() is salted per
        # process, so the same name would land on a different seed every run and
        # a "reproducible" reroll would not reproduce.
        prop_seed = (base_seed + zlib.crc32(name.encode()) % 100000) % 2**31
        # One queue slot renders all variants of this prop -- but only in
        # text2img. In img2img the sampler's latent comes from VAEEncode, which
        # always encodes exactly one image, so EmptyLatentImage.batch_size is
        # discarded and a batch of N silently yields 1. Variants there have to be
        # separate queues.
        batch = 1 if init_name else args.count
        graph = build_prompt(subject, prop_seed, steps=args.steps, cfg=args.cfg,
                             width=args.size, height=args.size,
                             lora_scale=args.lora_scale, style=args.style,
                             sampler=args.sampler, scheduler=args.scheduler,
                             init_image=init_name, denoise=args.denoise,
                             checkpoint=args.checkpoint, lora=lora_name,
                             trigger=trigger)
        graph["5"]["inputs"]["batch_size"] = batch

        variant_index = 0
        for queue_index in range(0, args.count, batch):
            # Distinct seed per queue so the variants are actually different
            # images rather than the same one repeated.
            if queue_index:
                graph["6"]["inputs"]["seed"] = (prop_seed + queue_index) % 2**31
            print(f"[{name}] queueing variant batch {queue_index // batch + 1} "
                  f"(seed {graph['6']['inputs']['seed']})")
            try:
                result = queue(graph, client_id)
            except urllib.error.HTTPError as exc:
                body = exc.read().decode(errors="replace")
                sys.exit(f"ComfyUI rejected the prompt ({exc.code}): {body}")
            prompt_id = result["prompt_id"]

            outputs = wait_for(prompt_id, client_id)
            images = outputs.get("8", {}).get("images", [])
            if not images:
                print(f"[{name}] warning: no images in outputs")
                continue

            for meta in images:
                blob = fetch_image(meta["filename"], meta.get("subfolder", ""),
                                   meta.get("type", "output"))
                raw_path = os.path.join(raw_dir, f"{name}_{variant_index}_raw.png")
                final_path = os.path.join(args.out_dir, f"{name}_{variant_index}.png")
                with open(raw_path, "wb") as handle:
                    handle.write(blob)
                with open(final_path, "wb") as handle:
                    handle.write(blob)
                manifest.append({
                    "prop": name,
                    "variant": variant_index,
                    "seed": graph["6"]["inputs"]["seed"],
                    "subject": subject,
                    "raw": os.path.relpath(raw_path, ROOT),
                    "file": os.path.relpath(final_path, ROOT),
                })
                print(f"[{name}] wrote {os.path.relpath(final_path, ROOT)} "
                      f"({len(blob)} bytes)")
                variant_index += 1
            if variant_index >= args.count:
                break

    if manifest:
        path = os.path.join(args.out_dir, "manifest.json")
        with open(path, "w") as handle:
            json.dump({"base_seed": base_seed, "params": {
                "checkpoint": args.checkpoint, "lora": lora_name,
                "trigger": trigger,
                "lora_scale": args.lora_scale, "steps": args.steps,
                "cfg": args.cfg, "size": args.size,
            }, "images": manifest}, handle, indent=2)
        print(f"\nwrote {os.path.relpath(path, ROOT)}")
        print("next: python3 sprite_preview/comfy_sheet.py  "
              "(contact sheet at true game scale)")


if __name__ == "__main__":
    main()
