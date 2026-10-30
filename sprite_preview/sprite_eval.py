#!/usr/bin/env python3
"""Ask a vision LLM to judge generated sprites, and measure whether it is right.

Scratch tooling; not part of the game build.

Why this exists
---------------
Curation is the real bottleneck: ~100 sprites have been looked at by eye to pick
11. A multimodal judge could remove that cost, but only if its judgement actually
agrees with a human's -- and there is already evidence that proxy metrics do NOT.
`compare_real.py` reports colour count and exact-repeat rate, and the curated
sprites' numbers sit right next to the real art pack's while the art clearly does
not. So this tool does not assume a VLM will be better. It measures it.

The measurement is **agreement with ground truth**, not a score. `curated.json`
records which variant was hand-picked for each prop, so every prop with N variants
is a small labelled question. A judge that cannot pick the same winner is not
useful, however confident its prose sounds.

Three things are tested, in increasing order of how much they would matter:

1. **Calibration.** The real art pack is included in the sheet. If the judge
   ranks generated sprites above the hand-made ones, its notion of "good" is
   wrong for this game and nothing else it says can be trusted. This is the
   control, and it is the first thing reported.
2. **Winner-picking.** Does its top pick match `curated.json`?
3. **Failure detection.** Does it notice the specific defects that were found by
   eye -- fragments, scene clutter, unreadable silhouettes?

Presenting the sprite
---------------------
How a 32x32 sprite is shown to a VLM is a real variable, not a detail. A 32px
PNG is tiny in token terms and may be effectively invisible; a 6x nearest-
neighbour upscale is what a human reviewer sees. `--zoom` tests both, because the
judge may well be better at one.

Usage:
    # free: build the sheet and print the exact prompt
    python3 sprite_eval.py --prop barrel --variants f_final256 --dry-run

    # the real thing (costs one API call per prop)
    python3 sprite_eval.py --all --model typesafe/jev-router

    # compare judges, and both zoom levels
    python3 sprite_eval.py --all --model typesafe/jev-router --zoom 6
"""

import argparse
import base64
import io
import json
import os
import re
import sys
import urllib.error
import urllib.request

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow missing; use sprite_preview/run.sh")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_DIR = os.path.join(HERE, "comfy_out")
GAME = os.path.join(ROOT, "data", "sprites")
CURATED = os.path.join(HERE, "curated.json")
MODEL_CONFIG = os.path.join(ROOT, "model.json")

API = "https://openrouter.ai/api/v1/chat/completions"
DEFAULT_MODEL = "typesafe/jev-router"

# The judgement schema. Enforced via response_format rather than requested in
# prose, because asking politely does not work: with a loose "reply with JSON"
# instruction, one reply spent its entire 1200-token budget on hidden reasoning
# and returned EMPTY content (usage showed reasoning_tokens=1200,
# completion_tokens=1200), and another was truncated mid-object.
#
# An earlier version of this schema carried a free-text `notes` field. That was a
# mistake worth recording: it created a prose channel, and prose read from it was
# then mistaken for the model volunteering explanation. Jev returns structured
# output, so the schema now contains typed fields only, and every claim about a
# judge has to come from one of them.
#
# There is deliberately NO "best" field and no ranking. v3 asked for a ranking
# *and* a rejection flag, and the two conflicted: the judge resolved it by always
# crowning a winner, including on props where every variant was unusable. A
# categorical per-candidate verdict makes "reject" representable on its own.
JUDGEMENT_SCHEMA = {
    "type": "object",
    "properties": {
        "verdicts": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "cell": {"type": "integer"},
                    # The categorical call. Deliberately three-way and
                    # deliberately NOT accompanied by a "best" field, because a
                    # forced ranking fights a rejection: v3 of this schema asked
                    # for both and the judge resolved the conflict by always
                    # crowning something, including on props where the owner
                    # judged every variant unusable.
                    "verdict": {"type": "enum",
                                "enum": ["keep", "usable", "reject"]},
                    "recognisable": {"type": "integer"},
                    "clean": {"type": "integer"},
                },
                "required": ["cell", "verdict", "recognisable", "clean"],
                "additionalProperties": False,
            },
        },
        # Explicit, so "nothing here is good enough" is representable without
        # contradicting any other field.
        "any_acceptable": {"type": "boolean"},
        "reference_beats_all": {"type": "boolean"},
    },
    "required": ["verdicts", "any_acceptable", "reference_beats_all"],
    "additionalProperties": False,
}

# The reference sprites shown alongside the candidates. These are the style
# target and the calibration control: a judge that ranks generated sprites above
# these is not calibrated for this game, whatever else it says.
REFERENCE = ["apple", "bread", "anvil", "chest"]

# Variant directories, newest/best first per curated provenance.
VARIANT_DIRS = ("f_final256", "f_q256", "f_v3", "f_aziib", "final512")


def api_key():
    with open(MODEL_CONFIG) as handle:
        key = json.load(handle).get("apiKey") or ""
    if not key:
        sys.exit("model.json has no apiKey")
    return key


def load_rgba(path):
    with Image.open(path) as img:
        return img.convert("RGBA")


def on_bg(sprite, size=32, bg=(24, 20, 28)):
    canvas = Image.new("RGB", (size, size), bg)
    canvas.paste(sprite, (0, 0), sprite)
    return canvas


def grid_image(items, zoom, cols=4):
    """items: list of (label, RGBA). Returns a base64 PNG plus the labels.

    Each cell is labelled with a big, unambiguous number. Asking a VLM to refer
    to "the second one" is unreliable; asking it to emit an integer printed
    inside the cell is not.
    """
    cell = 32 * zoom
    pad = 14
    label_h = 26
    rows = (len(items) + cols - 1) // cols
    width = cols * (cell + pad) + pad
    height = rows * (cell + label_h + pad) + pad + 30
    canvas = Image.new("RGB", (width, height), (18, 18, 22))
    draw = ImageDraw.Draw(canvas)
    try:
        big = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 20)
        small = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 13)
    except OSError:
        big = small = ImageFont.load_default()

    draw.text((pad, 6), "Cells are numbered. Background is the game's dark "
              "sprite backdrop.", fill=(200, 200, 210), font=small)

    labels = []
    for index, (label, sprite) in enumerate(items):
        col = index % cols
        row = index // cols
        x = pad + col * (cell + pad)
        y = pad + 30 + row * (cell + label_h + pad)
        canvas.paste(on_bg(sprite).resize((cell, cell), Image.NEAREST), (x, y))
        draw.rectangle([x, y, x + cell, y + cell], outline=(90, 90, 105))
        # Number badge in the corner, over the sprite.
        draw.rectangle([x, y, x + 30, y + 26], fill=(0, 0, 0))
        draw.text((x + 6, y + 1), str(index + 1), fill=(255, 235, 120), font=big)
        draw.text((x, y + cell + 3), label[:26], fill=(205, 205, 215),
                  font=small)
        labels.append(label)

    buf = io.BytesIO()
    canvas.save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode(), labels, canvas


def build_prompt(prop, labels, n_reference):
    n_cells = len(labels) - n_reference
    return f"""You are art-directing a 2D pixel-art game. You will see numbered \
sprite cells for the game object "{prop}", plus {n_reference} reference sprites \
at the start labelled REFERENCE.

The REFERENCE sprites are hand-made by the game's artist and define the quality \
bar. Compare every candidate against that bar in ABSOLUTE terms. Being the best \
of {n_cells} does not make a cell good, and most cells in a typical batch are \
not good enough to ship.

Cells are numbered 1..{len(labels)}.

For EVERY candidate cell (not the REFERENCE cells) give:
- "verdict": exactly one of
    "keep"   -- you would ship this sprite as-is. Clean single object, clearly \
reads as {prop}, nothing unwanted attached.
    "usable" -- flawed but salvageable with work (minor clutter, slightly weak \
silhouette). Not shippable as-is.
    "reject" -- do not ship. Attached scenery, decoration, extra objects, a \
fragment, or it does not read as {prop} at all.
- "recognisable": 0-10, does it clearly read as {prop}?
- "clean": 0-10, is it ONLY the object, with no scenery, ground, buildings, \
plants, fences or extra props attached? 10 means nothing unwanted.

Then set "any_acceptable" to true only if at least one candidate is "keep". \
It is a correct and expected answer for all candidates to be "usable" or \
"reject"; do not feel obliged to promote anything.

Set "reference_beats_all" to true if the REFERENCE sprites look better than \
every candidate.

Reply with ONLY a JSON object:
{{"verdicts": [{{"cell": {n_reference + 1}, "verdict": "reject", "recognisable": 0, \
"clean": 0}}], "any_acceptable": false, "reference_beats_all": true}}"""


def call_vision(model, prompt, image_b64, key, timeout=300, enforce_schema=True):
    payload = {
        "model": model,
        "messages": [{
            "role": "user",
            "content": [
                {"type": "text", "text": prompt},
                {"type": "image_url",
                 "image_url": {"url": "data:image/png;base64," + image_b64}},
            ],
        }],
        # Generous, because reasoning tokens are billed and budgeted separately
        # from the answer and a reasoning model can spend thousands before
        # emitting a single JSON character. 1200 was the cause of a total miss.
        "max_tokens": 8000,
        "temperature": 0,
    }
    if enforce_schema:
        # Jev advertises `structured_outputs` and `response_format`, so the shape
        # is guaranteed by the API rather than by hoping. Models that reject the
        # parameter fall through to the prompt-only path (see the retry below).
        payload["response_format"] = {
            "type": "json_schema",
            "json_schema": {
                "name": "sprite_judgement",
                "strict": True,
                "schema": JUDGEMENT_SCHEMA,
            },
        }
    req = urllib.request.Request(
        API, data=json.dumps(payload).encode(),
        headers={"Authorization": "Bearer " + key,
                 "Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = json.loads(resp.read().decode())
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode(errors="replace")[:600]
        # Not every model accepts response_format. Fall back to the prompt-only
        # contract once, rather than failing the whole batch.
        if enforce_schema and exc.code == 400:
            return call_vision(model, prompt, image_b64, key, timeout,
                               enforce_schema=False)
        sys.exit(f"HTTP {exc.code} from {model}:\n{detail}")

    choice = body["choices"][0]
    text = (choice["message"].get("content") or "")
    if isinstance(text, list):
        text = "".join(part.get("text", "") for part in text)
    usage = body.get("usage", {})
    if not text.strip():
        # Say WHY it was empty. A reasoning model that exhausts its budget before
        # emitting the answer returns content=null with the tokens spent on
        # `reasoning`; reporting that as a bare "unparsable" hides the fix.
        reason = choice.get("finish_reason")
        spent = (usage.get("completion_tokens_details") or {}).get(
            "reasoning_tokens")
        text = (f"[EMPTY content. finish_reason={reason} "
                f"reasoning_tokens={spent} "
                f"completion_tokens={usage.get('completion_tokens')}]")
    return text, usage


def parse_json(text):
    """Pull the JSON object out of a reply that may be chatty despite the ask."""
    fenced = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.S)
    candidate = fenced.group(1) if fenced else None
    if candidate is None:
        start = text.find("{")
        end = text.rfind("}")
        if start >= 0 and end > start:
            candidate = text[start:end + 1]
    if candidate is None:
        return None
    try:
        return json.loads(candidate)
    except json.JSONDecodeError:
        return None


def my_pick(prop):
    """The hand-made choice from curated.json, if one is recorded."""
    if not os.path.exists(CURATED):
        return None
    with open(CURATED) as handle:
        return (json.load(handle).get("sprites") or {}).get(prop)


def find_variants(prop, directory, shuffle_seed=None):
    """All finished variants for a prop in a directory, sorted by index."""
    full = os.path.join(OUT_DIR, directory)
    if not os.path.isdir(full):
        return []
    found = []
    for name in sorted(os.listdir(full)):
        if not name.endswith(".png"):
            continue
        stem = name[:-4]
        # Variants are named <prefix>_<prop>_<n> after tagging, or <prop>_<n>.
        bare = stem
        for prefix in ("f_", "h_", "q256_", "q128_", "v3_", "az_", "b2_", "lk_"):
            if bare.startswith(prefix):
                bare = bare[len(prefix):]
                break
        base, _, index = bare.rpartition("_")
        if base == prop and index.isdigit():
            found.append((int(index), stem))
    stems = [stem for _, stem in sorted(found)]
    if shuffle_seed is not None:
        # Deterministic permutation keyed on the prop name, so the two runs differ
        # in order for each prop individually rather than all in lockstep.
        import random
        rng = random.Random(f"{shuffle_seed}:{prop}")
        rng.shuffle(stems)
    return stems


def evaluate(prop, directory, model, key, zoom, dry_run, cols,
             shuffle_seed=None):
    stems = find_variants(prop, directory, shuffle_seed)
    if not stems:
        return None

    items = []
    for name in REFERENCE:
        path = os.path.join(GAME, name + ".png")
        if os.path.exists(path):
            items.append(("REFERENCE " + name, load_rgba(path)))
    n_reference = len(items)
    for stem in stems:
        items.append((stem, load_rgba(os.path.join(OUT_DIR, directory,
                                                   stem + ".png"))))

    image_b64, labels, canvas = grid_image(items, zoom, cols)
    prompt = build_prompt(prop, labels, n_reference)

    if dry_run:
        sheet = os.path.join(OUT_DIR, f"eval_sheet_{prop}.png")
        canvas.save(sheet)
        print(f"--- {prop}: {len(stems)} variant(s) + {n_reference} reference")
        print(f"    sheet: {os.path.relpath(sheet, ROOT)} "
              f"({canvas.width}x{canvas.height})")
        print(f"    labels: {labels}")
        print(prompt)
        print()
        return None

    text, usage = call_vision(model, prompt, image_b64, key)
    parsed = parse_json(text)
    if parsed is None:
        print(f"[{prop}] could not parse a JSON reply. Raw start:\n{text[:300]}")
        return {"prop": prop, "error": "unparsable", "raw": text,
                "stems": stems, "usage": usage}

    # Map the judge's cell numbers back to stems. Cells 1..n_reference are the
    # reference sprites, so a verdict on a cell at or below that count is out of
    # range and is dropped rather than attributed to a real candidate.
    result = {"prop": prop, "stems": stems, "n_reference": n_reference,
              "parsed": parsed, "usage": usage}
    by_cell = {}
    for entry in parsed.get("verdicts", []):
        cell = entry.get("cell")
        if isinstance(cell, int) and n_reference < cell <= len(labels):
            by_cell[labels[cell - 1]] = entry
    result["verdicts_by_stem"] = by_cell
    # The judge's own "would I ship anything here" answer, and (for reporting)
    # which stems it said were shippable.
    result["any_acceptable"] = parsed.get("any_acceptable")
    result["keep_stems"] = [stem for stem, e in by_cell.items()
                            if e.get("verdict") == "keep"]
    result["reference_beats_all"] = parsed.get("reference_beats_all")
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--prop", action="append", default=[],
                    help="prop to evaluate (repeatable)")
    ap.add_argument("--all", action="store_true",
                    help="every prop in curated.json")
    ap.add_argument("--variants", default="f_final256",
                    help="directory of finished variants under comfy_out/")
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--zoom", type=int, default=6,
                    help="sprite upscale before sending (1 tests raw 32px)")
    ap.add_argument("--cols", type=int, default=4)
    ap.add_argument("--dry-run", action="store_true",
                    help="build sheets and print prompts, make no API call")
    ap.add_argument("--out", default=os.path.join(OUT_DIR, "eval_results.json"))
    ap.add_argument("--shuffle-seed", type=int, default=None,
                    help="deterministically permute variant order. Run twice with "
                         "different seeds: a judge that picks the same STEM both "
                         "times is judging the sprite; one that follows position "
                         "is judging the layout")
    args = ap.parse_args()

    props = list(args.prop)
    if args.all:
        with open(CURATED) as handle:
            props = sorted((json.load(handle).get("sprites") or {}).keys())
    if not props:
        sys.exit("give --prop NAME or --all")

    key = None if args.dry_run else api_key()

    results = []
    for prop in props:
        out = evaluate(prop, args.variants, args.model, key, args.zoom,
                       args.dry_run, args.cols, args.shuffle_seed)
        if out:
            results.append(out)

    if args.dry_run:
        return

    # ---- scoring ------------------------------------------------------
    with open(args.out, "w") as handle:
        json.dump({"model": args.model, "variants": args.variants,
                   "zoom": args.zoom, "shuffle_seed": args.shuffle_seed,
                   "results": results}, handle, indent=2)

    print(f"\nmodel: {args.model}   variants: {args.variants}   "
          f"zoom: {args.zoom}x")
    print(f"{'prop':14s} {'verdicts (keep/usable/reject)':30s} "
          f"{'any?':5s} {'verdict':9s} ref-wins")
    print("-" * 80)
    beats_all_true = beats_all_n = 0
    recused = 0
    for res in results:
        prop = res["prop"]
        if res.get("error"):
            print(f"{prop:14s} PARSE ERROR")
            continue
        by = res.get("verdicts_by_stem", {})
        counts = {"keep": 0, "usable": 0, "reject": 0}
        for entry in by.values():
            v = entry.get("verdict")
            if v in counts:
                counts[v] += 1
        any_acc = res.get("any_acceptable")
        if any_acc is False:
            recused += 1
        if res.get("reference_beats_all") is True:
            beats_all_true += 1
        if res.get("reference_beats_all") is not None:
            beats_all_n += 1
        # "verdict" column: is the judge's own summary consistent with its levels?
        keeps = counts["keep"]
        consistent = (keeps > 0) == bool(any_acc)
        print(f"{prop:14s} {counts['keep']}/{counts['usable']}/{counts['reject']}"
              f"{'':22s} {str(any_acc):5s} "
              f"{'ok' if consistent else 'INCONSISTENT':9s} "
              f"{str(res.get('reference_beats_all'))}")

    print("-" * 80)
    print(f"judge refused to promote anything: {recused}/{len(results)} props")
    if beats_all_n:
        print(f"judge said REFERENCE beats all candidates: "
              f"{beats_all_true}/{beats_all_n}")
    print(f"\nwrote {os.path.relpath(args.out, ROOT)}")
    print("Score this against human_grades.json with judge_scorecard.py -- "
          "the verdict column here only checks the judge against itself.")


if __name__ == "__main__":
    main()
