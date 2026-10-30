#!/usr/bin/env python3
"""Score generated sprites with PickScore, and check the scores against real grades.

Scratch tooling; not part of the game build.

What PickScore is, and why it is a different kind of tool
--------------------------------------------------------
PickScore (`yuvalkirstain/PickScore_v1`) is a CLIP ViT-H/14 fine-tuned on human
A/B preferences from Pick-a-Pic. It scores a **text-image pair**: high means "a
human would probably prefer this image for this prompt". It is not a VQA model --
it cannot say *what* is in the image, only how well the image matches the prompt
and how appealing it is. So it can produce the quality grade that was asked for,
but not the identification, and this tool does not pretend otherwise.

The score is a raw logit and the absolute number is arbitrary; only comparisons
matter. Empirically on this model a strong match lands ~20-23, a weak one ~17-19.
Do not read the number as a percentage or a mark out of anything.

Setup (NixOS, no system torch)
------------------------------
    # one-off, already done on this machine:
    nix-shell -p python3Packages.python python3Packages.pip --run \
        'python3 -m venv /tmp/ps-venv && /tmp/ps-venv/bin/python -m pip install \
         torch --index-url https://download.pytorch.org/whl/cpu && \
         /tmp/ps-venv/bin/python -m pip install transformers Pillow'

torch needs a 64-bit libstdc++ that is not on the default path, and the HF cache
must be somewhere writable (`~/.cache` is not, under the sandbox). Both are
handled by `run_pickscore.sh`, which is the only supported way to invoke this.

Validation, not just scoring
----------------------------
Producing numbers for a batch is not evidence that the numbers mean anything --
this project has already shipped two scorers that returned compressed, constant
values. So this tool has a `--validate` mode that scores a set of sprites whose
quality a human already graded (`human_grades.json`), and reports whether the
scores actually separate good from bad. That is the question that decides whether
PickScore is worth using at all.

Usage:
    sprite_preview/run_pickscore.sh pickscore_eval.py --images <dir> --prop fireplace
    sprite_preview/run_pickscore.sh pickscore_eval.py --validate
"""

import argparse
import json
import os
import statistics
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow missing; use run_pickscore.sh")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_DIR = os.path.join(HERE, "comfy_out")
GRADES = os.path.join(HERE, "human_grades.json")

MODEL_ID = "yuvalkirstain/PickScore_v1"

# A prompt template rather than one hardcoded string: PickScore scores
# text-image alignment, so the wording is part of the measurement. The
# "pixel art game sprite of" framing matches how the art was actually asked for.
PROMPT_TEMPLATE = "a pixel art game sprite of a {subject}"

# Controls used to prove the scorer is reading the image and not just echoing the
# prompt. A matching prompt must beat a mismatched one on the same image; if it
# does not, the scores are noise.
CONTROL_WRONG = "a photograph of a cat"


def load_model():
    """Load PickScore once. Imported lazily so --help works without torch."""
    import torch
    from transformers import AutoProcessor, AutoModel

    print(f"loading {MODEL_ID} ...", flush=True)
    processor = AutoProcessor.from_pretrained(MODEL_ID)
    model = AutoModel.from_pretrained(MODEL_ID).eval()
    print("model loaded", flush=True)
    return processor, model, torch


def score(processor, model, torch, image, text):
    """PickScore logit for one image-text pair."""
    inputs = processor(text=[text], images=[image], return_tensors="pt",
                       padding=True, truncation=True, max_length=77)
    with torch.no_grad():
        out = model(**inputs)
    return float(out.logits_per_image[0][0])


def load_rgb(path):
    """PickScore is a CLIP model: RGB, and it resizes internally.

    Sprites are composited onto the game backdrop first so transparency does not
    become black, which would depress the score for reasons unrelated to art
    quality.
    """
    with Image.open(path) as raw:
        img = raw.convert("RGBA")
        flat = Image.new("RGB", img.size, (24, 20, 28))
        flat.paste(img, (0, 0), img)
        return flat


def collect(dirs):
    found = []
    for directory in dirs:
        if not os.path.isdir(directory):
            sys.exit(f"no such directory: {directory}")
        for name in sorted(os.listdir(directory)):
            if not name.endswith(".png"):
                continue
            if name.endswith("_sheet.png") or name.startswith("_"):
                continue
            found.append(os.path.join(directory, name))
    return found


def run_batch(args, processor, model, torch):
    paths = collect(args.images)
    if not paths:
        sys.exit("no images found")

    subject = args.prop.replace("_", " ")
    good_prompt = PROMPT_TEMPLATE.format(subject=subject)

    print(f"\nprompt : {good_prompt!r}")
    print(f"images : {len(paths)}")
    print(f"\n{'image':34s} {'score':>7s} {'wrong-prompt':>13s} {'gap':>7s}")
    print("-" * 66)

    rows = []
    for path in paths:
        img = load_rgb(path)
        good = score(processor, model, torch, img, good_prompt)
        wrong = score(processor, model, torch, img, CONTROL_WRONG)
        rows.append({"file": os.path.relpath(path, ROOT), "score": good,
                     "wrong_prompt_score": wrong, "gap": good - wrong})
        print(f"{os.path.basename(path):34s} {good:7.3f} {wrong:13.3f} "
              f"{good - wrong:7.3f}")

    if rows:
        gaps = [r["gap"] for r in rows]
        print("-" * 66)
        print(f"mean score {statistics.mean(r['score'] for r in rows):.3f}   "
              f"mean gap {statistics.mean(gaps):.3f}")
        if min(gaps) <= 0:
            print("WARNING: at least one image scored no better on the matching "
                  "prompt than on 'a photograph of a cat'. Those scores are not "
                  "measuring what you think.")
        else:
            print("control check: every image scores higher on the matching "
                  "prompt than on a nonsense one -- the scores track content.")

    payload = {"model": MODEL_ID, "prompt": good_prompt,
               "control_prompt": CONTROL_WRONG, "items": rows}
    with open(args.out, "w") as handle:
        json.dump(payload, handle, indent=2)
    print(f"\nwrote {os.path.relpath(args.out, ROOT)}")
    return rows


def run_validate(processor, model, torch):
    """Does the score separate sprites a human called good from ones they called bad?

    Uses `human_grades.json`, which records the owner's grade for every variant on
    the first judge sheet. If PickScore is worth using, mean(good) > mean(ok) >
    mean(bad). If those means are indistinguishable, the scorer is decorative --
    exactly the failure found in Jev, Gemini and moondream, where every image got
    a near-identical mark.
    """
    with open(GRADES) as handle:
        grades = json.load(handle)["grades"]

    src = os.path.join(OUT_DIR, "f_final256")
    if not os.path.isdir(src):
        sys.exit(f"validation source missing: {src}")

    by_bucket = {"good": [], "ok": [], "bad": []}
    print(f"\n{'prop':14s} {'variant':6s} {'your grade':11s} {'score':>8s}")
    print("-" * 44)
    for prop, entry in sorted(grades.items()):
        subject = prop.replace("_", " ")
        prompt = PROMPT_TEMPLATE.format(subject=subject)
        for variant, grade in sorted(entry["grades"].items()):
            # Variant keys are 1-based sheet numbers; files are 0-based.
            index = int(variant) - 1
            path = os.path.join(src, f"f_{prop}_{index}.png")
            if not os.path.exists(path):
                continue
            value = score(processor, model, torch, load_rgb(path), prompt)
            by_bucket[grade].append(value)
            print(f"{prop:14s} {variant:6s} {grade:11s} {value:8.3f}")

    print("-" * 44)
    print(f"{'graded bucket':14s} {'n':>3s} {'mean':>8s} {'min':>8s} {'max':>8s}")
    for bucket in ("good", "ok", "bad"):
        values = by_bucket[bucket]
        if not values:
            continue
        print(f"{bucket:14s} {len(values):3d} {statistics.mean(values):8.3f} "
              f"{min(values):8.3f} {max(values):8.3f}")

    good = by_bucket["good"]
    bad = by_bucket["bad"]
    if good and bad:
        gap = statistics.mean(good) - statistics.mean(bad)
        print(f"\nmean(good) - mean(bad) = {gap:+.3f}")
        overlaps = sum(1 for b in bad if b > min(good))
        print(f"bad sprites scoring above the worst good sprite: "
              f"{overlaps}/{len(bad)}")
        if gap > 0.5:
            print("Verdict: the scores DO separate hand-graded good from bad.")
        elif gap > 0:
            print("Verdict: separation is positive but small; treat as weak "
                  "signal only.")
        else:
            print("Verdict: no separation. Do not use this to pick sprites.")

    out = os.path.join(OUT_DIR, "pickscore_validation.json")
    with open(out, "w") as handle:
        json.dump({b: v for b, v in by_bucket.items()}, handle, indent=2)
    print(f"\nwrote {os.path.relpath(out, ROOT)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--images", nargs="+", default=[],
                    help="directories of PNGs to score")
    ap.add_argument("--prop", default="fireplace",
                    help="subject used in the scoring prompt")
    ap.add_argument("--out", default=os.path.join(OUT_DIR, "pickscore_scores.json"))
    ap.add_argument("--validate", action="store_true",
                    help="score human-graded sprites and test separation")
    args = ap.parse_args()

    if not args.validate and not args.images:
        sys.exit("give --images DIR or --validate")

    processor, model, torch = load_model()

    if args.images:
        run_batch(args, processor, model, torch)
    if args.validate:
        run_validate(processor, model, torch)


if __name__ == "__main__":
    main()
