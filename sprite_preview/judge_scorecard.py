#!/usr/bin/env python3
"""Score a VLM judge's verdicts against the owner's grades.

Scratch tooling; not part of the game build.

Why this is a separate tool from sprite_eval.py
-----------------------------------------------
`sprite_eval.py` only checks a judge against itself -- it can report what the
judge said but not whether it was right. The reference judgement lives in
`human_grades.json` (the owner's grading of `judge_sheet.png`), and this tool is
the only place that compares the two. Keeping them apart also means a new judge
run is free to score: no re-rendering, no API calls.

The metrics, and why there is more than one
-------------------------------------------
A single accuracy number hid the real behaviour twice in this project, in
opposite directions, so three are reported:

  * **top-pick accuracy** -- on props where the owner named a best variant, did
    the judge name the same one? This is the "choosing" capability.
  * **usable-pick rate** -- how often the judge promoted something the owner
    called good or ok. This is the "not wasting the owner's time" property.
  * **rejection behaviour**, reported as a 2x2: the owner's all-bad props versus
    props with something usable. A judge that never rejects cannot screen a
    batch; a judge that always rejects is useless. Only the confusion matrix
    shows which failure it has. An earlier run looked fine on accuracy while
    silently never refusing a single all-bad prop.

Usage:
    sprite_preview/run.sh judge_scorecard.py eval_jev2.json eval_gemini_s11.json
    sprite_preview/run.sh judge_scorecard.py --all-runs   # every eval_*.json
"""

import argparse
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_DIR = os.path.join(HERE, "comfy_out")
GRADES = os.path.join(HERE, "human_grades.json")
GRADES_FRESH = os.path.join(HERE, "human_grades_fresh.json")

PREFIXES = ("f_", "v3_", "az_", "q256_", "q128_", "h_", "lk_", "b2_")

# Human grade -> the credit a judge earns for promoting that variant.
CREDIT = {"good": 1.0, "ok": 0.5, "bad": 0.0}


def load_grades(source=None):
    """Load a ground-truth file, normalised to {prop: {accept:[stems], all_bad}}.

    Two schemas exist. The first sheet recorded a single best variant by 1-based
    index (`human_grades.json`); the fresh set recorded a set of acceptable
    variants by stem (`human_grades_fresh.json`), because some props genuinely
    have two shippable variants and forcing one "best" would have been a fiction.
    Normalising here keeps the metrics comparable across both.
    """
    path = source or GRADES
    if not os.path.isabs(path) and not os.path.exists(path):
        path = os.path.join(HERE, path)
    with open(path) as handle:
        raw = json.load(handle)["grades"]

    out = {}
    for prop, g in raw.items():
        if "accept" in g:
            out[prop] = {"accept": list(g["accept"]),
                         "all_bad": bool(g["all_bad"])}
        else:
            # Legacy index schema -> stems are resolved by the caller, which
            # knows the prefix; represent the single best as accept=None and
            # compare on index instead.
            out[prop] = {"accept": None,
                         "best_index": g.get("best"),
                         "all_bad": g.get("best") is None}
    return out


def strip_prefix(stem):
    for prefix in PREFIXES:
        if stem.startswith(prefix):
            return stem[len(prefix):]
    return stem


def variant_index(stem):
    """Return the 1-based variant number a judge's chosen stem refers to."""
    if not stem:
        return None
    bare = strip_prefix(stem)
    tail = bare.rsplit("_", 1)
    return int(tail[1]) + 1 if len(tail) == 2 and tail[1].isdigit() else None


def score_run(path, grades):
    with open(path) as handle:
        data = json.load(handle)

    rows = []
    for res in data.get("results", []):
        prop = res["prop"]
        grade = grades.get(prop)
        if not grade:
            continue
        if res.get("error"):
            rows.append({"prop": prop, "error": res["error"]})
            continue

        # Chosen stem: the old schema had "best_label"; the categorical schema
        # has a verdict list. Support both so historical runs stay comparable.
        chosen = None
        if res.get("best_label"):
            chosen = res["best_label"]
        elif res.get("keep_stems"):
            chosen = res["keep_stems"][0]

        promoted = list(res.get("keep_stems") or [])
        # Old-schema runs promoted exactly one thing: their "best".
        if not promoted and chosen:
            promoted = [chosen]

        # Credit: how much the human values what the judge promoted.
        #   fresh schema  -> 1.0 if inside the acceptable set, else 0.0
        #   legacy schema -> graded credit by that variant's good/ok/bad label
        credited = []
        for stem in promoted:
            if grade.get("accept") is not None:
                credited.append(1.0 if stem in grade["accept"] else 0.0)
            else:
                index = variant_index(stem)
                credited.append(
                    CREDIT.get(grade.get("grades", {}).get(str(index)), 0.0))

        rejected_all = res.get("parsed", {}).get("recuse") is True or \
            res.get("parsed", {}).get("any_acceptable") is False

        # Two ways to be right: land inside the acceptable STEM set (fresh
        # schema), or match the single best INDEX (legacy schema).
        if grade.get("accept") is not None:
            hit = (chosen in grade["accept"]) if chosen else None
            human_best = ", ".join(x.split("_", 1)[-1] for x in grade["accept"]) \
                or None
        else:
            hit = (variant_index(chosen) == grade.get("best_index")
                   if chosen and grade.get("best_index") else None)
            human_best = grade.get("best_index")

        rows.append({
            "prop": prop,
            "chosen": chosen,
            "index": variant_index(chosen),
            "best": human_best,
            "has_good": not grade["all_bad"],
            "hit": hit,
            "credit": max(credited) if credited else None,
            "rejected_all": rejected_all,
            "reference_beats_all": res.get("reference_beats_all"),
        })
    return data.get("model", "?"), rows


def report(name, rows):
    scored = [r for r in rows if not r.get("error")]
    errors = len(rows) - len(scored)
    with_best = [r for r in scored if r["has_good"]]
    hits = sum(1 for r in with_best if r["hit"])
    allbad = [r for r in scored if not r["has_good"]]

    # Rejection behaviour, the 2x2 that matters for screening.
    refused_allbad = sum(1 for r in allbad if r["rejected_all"])
    refused_good = sum(1 for r in with_best if r["rejected_all"])

    credits = [r["credit"] for r in scored if r["credit"] is not None]
    mean_credit = sum(credits) / len(credits) if credits else float("nan")

    print(f"=== {name}")
    print(f"    parsed {len(scored)}/{len(rows)}"
          + (f"  ({errors} error(s))" if errors else ""))
    if with_best:
        print(f"    top-pick accuracy      {hits}/{len(with_best)} "
              f"({100.0 * hits / len(with_best):.0f}%)")
    if credits:
        print(f"    mean human credit      {mean_credit:.2f} "
              f"(1.0=good, 0.5=ok, 0.0=bad)")
    print(f"    refused on all-bad     {refused_allbad}/{len(allbad)}"
          "   <- screening: should be HIGH")
    print(f"    refused on usable      {refused_good}/{len(with_best)}"
          "   <- false rejection: should be LOW")
    for r in scored:
        if r["has_good"]:
            mark = "HIT" if r["hit"] else "miss"
            print(f"      {r['prop']:14s} judge {str(r['chosen']):>16s} | "
                  f"you {str(r['best']):>16s} | {mark}")
        else:
            print(f"      {r['prop']:14s} judge "
                  f"{'refused' if r['rejected_all'] else 'promoted something'}"
                  f" | you: none usable")
    print()
    return {
        "model": name,
        "top_pick": [hits, len(with_best)],
        "mean_credit": mean_credit,
        "refused_allbad": [refused_allbad, len(allbad)],
        "refused_usable": [refused_good, len(with_best)],
        "errors": errors,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("runs", nargs="*", help="eval_*.json under comfy_out/")
    ap.add_argument("--all-runs", action="store_true",
                    help="score every eval_*.json present")
    ap.add_argument("--grades", default=None,
                    help="ground truth file (default human_grades.json); use "
                         "human_grades_fresh.json for the unseen set")
    args = ap.parse_args()

    if args.all_runs:
        paths = sorted(glob.glob(os.path.join(OUT_DIR, "eval_*.json")))
    else:
        paths = []
        for item in args.runs:
            path = item if os.path.exists(item) else os.path.join(OUT_DIR, item)
            if not os.path.exists(path):
                sys.exit(f"no such run file: {item}")
            paths.append(path)
    if not paths:
        sys.exit("give one or more eval_*.json files, or --all-runs")

    grades = load_grades(args.grades)
    summaries = []
    for path in paths:
        name, rows = score_run(path, grades)
        summaries.append(report(f"{name}  [{os.path.basename(path)}]", rows))

    if len(summaries) > 1:
        print("=" * 78)
        print(f"{'run':46s} {'top-pick':>10s} {'credit':>7s} "
              f"{'ref/allbad':>11s} {'ref/usable':>11s}")
        print("-" * 78)
        for s in summaries:
            tp = (f"{s['top_pick'][0]}/{s['top_pick'][1]}"
                  if s["top_pick"][1] else "-")
            ra = (f"{s['refused_allbad'][0]}/{s['refused_allbad'][1]}"
                  if s["refused_allbad"][1] else "-")
            ru = (f"{s['refused_usable'][0]}/{s['refused_usable'][1]}"
                  if s["refused_usable"][1] else "-")
            print(f"{s['model'][:46]:46s} {tp:>10s} {s['mean_credit']:7.2f} "
                  f"{ra:>11s} {ru:>11s}")
        print()
        print("No single column is the answer. A usable judge needs high top-pick,")
        print("high credit, high refusal on all-bad AND low refusal on usable props.")


if __name__ == "__main__":
    main()
