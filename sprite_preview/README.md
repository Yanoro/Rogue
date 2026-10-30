# Local prop sprites via ComfyUI

Generating world-object sprites for AIRogue with the owner's local ComfyUI
instance. **Two checkpoints are supported:** a dedicated SD 1.5 pixel-art
checkpoint (`aziibpixelmix_v10`) which is recommended, and the original
`v1-5-pruned-emaonly` base plus the `PixelArtRedmond15V` LoRA, kept for
comparison. Section 3.5 has the measurements that justify the default. Scratch
tooling — nothing here is referenced by `CMakeLists.txt` and nothing here runs at
game runtime.

This is the ComfyUI counterpart to two existing paths and deliberately a third
one, so read the difference before adding to any of them:

| | `tools/gen_tile.py` | `sprite_preview/gen_object_sheet.py` | this |
|---|---|---|---|
| backend | local SD 1.5 + LoRA, in-process `diffusers` | OpenRouter (Gemini) | local ComfyUI, over HTTP |
| cost | free per image, minutes each | ~$0.039 per request | free per image, ~7 s each |
| shape | one request per name, many names | one giant sheet, all subjects | **one request per prop, batch-sampled** |
| output | `data/sprites/` | `sprite_preview/raw/` | `sprite_preview/comfy_out/` |
| review | contact sheet | contact sheet | contact sheet + numeric gate |

The reason this path exists: local generation is free per image, so the right
shape is *not* one careful sheet. It is many cheap attempts with a batch sampler,
so a single bad prop can be rerolled for a few seconds instead of re-rolling a
sheet. The Gemini path is the opposite — per-request billing forces batching.

That property also drove the main conclusion: because attempts are free, **the
model is worth more than the graph**. Swapping the checkpoint beat every prompt
and sampling change combined (§3.5), and the same logic argues for curating many
cheap candidates over engineering one perfect pipeline.

---

## 1. Running it

ComfyUI must be up on `127.0.0.1:8188` (override with `COMFY_URL`). Pillow comes
from nix-shell via the wrapper — on NixOS the system Python cannot import it.

**Use the dedicated pixel-art checkpoint, not the base model plus LoRA.** This is
the single largest quality decision in the whole pipeline; see section 4 for the
measurements. The recommended invocation:

```bash
python3 sprite_preview/comfy_props.py \
    --checkpoint aziibpixelmix_v10.safetensors --no-lora \
    --style v3 --count 3 --seed 45000 \
    barrel well cart wheelbarrow water_trough wood_pile

# then finish (NEAREST is now the default; 256px renders are the default size)
./sprite_preview/run.sh finish_props.py --alpha-cut 0.3
```

Three defaults carry the quality, and all three were arrived at by looking at the
output at **1x** rather than at 6x:

- **`--size 256`** (not 512). The model's pixel-art grid is ~half the canvas, so
  256 gives a ~128px grid and 32px output stays crisp. At 512 the grid is ~256px
  and the 8:1 downscale turns to mush. §3.6.
- **NEAREST downscale** (not Lanczos). §3.6.
- **`--style v3`** for object isolation. §1.

`--no-lora` is deliberate: AziibPixelMix already contains the pixel-art style, and
stacking `PixelArtRedmond15V` on top fights it.

The original base+LoRA configuration still works and is kept for comparison:

```bash
python3 sprite_preview/comfy_props.py --style v2 --count 2 barrel well
```

```bash
# free: what props exist
python3 sprite_preview/comfy_props.py --list

# free: build the prompts and the graph, contact nothing
python3 sprite_preview/comfy_props.py --dry-run --no-lora barrel well cart

# reroll one prop that came out wrong
python3 sprite_preview/comfy_props.py --no-lora --style v3 barrel --count 4 --seed 9001
```

### Prompt templates (`--style`)

The prompt is not portable between checkpoints, which is why there is more than
one template.

| style | for | notes |
|---|---|---|
| `v1` | nothing | the original; asks for "chunky readable pixels", which makes the model pixelate the whole canvas. Kept as the falsified baseline |
| `v2` | base + LoRA | the first fix; no pixel language, empty flat background |
| `v3` | **AziibPixelMix** | forces a single object: "one object only, isolated object, no background scene … item icon", plus a heavy scene-suppressing negative |

`v3` exists because AziibPixelMix is a *scene* model. It gets the style right and
the composition wrong: asked for "a wooden cart" under `v2` it paints a cart in a
street with a fence, a tree and a dirt path, and the sprite becomes a diorama.
`v3` says "object" repeatedly because that word pulls the model from
*illustration* toward *asset*.

`--dry-run` is the cheap first step and should be used before any batch, the same
rule as `tools/README.md` §3.

Then finish and review:

```bash
# 512px renders -> 32x32 RGBA sprites + a contact sheet vs the real art pack
./sprite_preview/run.sh finish_props.py

# numbers only: colour count and edge hardness against the real props
./sprite_preview/run.sh compare_real.py sprite_preview/comfy_out/final/well_0.png
```

## 2. The pipeline, and where each stage lives

```
comfy_props.py      512x512 render, batch_size = variant count
      |             ComfyUI keeps its own copy in output/ as the raw render
      v
comfy_out/raw512/   the raw 512s -- never delete these
      |
      v
finish_props.py     background flood-fill -> alpha matte -> crop -> 32x32 RGBA
      |
      v
comfy_out/final/    finished sprites, ready to be promoted by hand
```

Five tools, each independently re-runnable, because **the finishing stage is
free and the rendering stage is not**. Every parameter in `finish_props.py` can be
retried twenty times for nothing; a re-render costs GPU time. So the raw renders
are kept forever (`tools/README.md` §11.2) and finishing is a separate pass.

| tool | job |
|---|---|
| `comfy_props.py` | build prompts, queue the graph, fetch results |
| `finish_props.py` | raw render -> finished 32×32 sprite; rejects unfixable input |
| `comfy_sheet.py` | contact sheet of candidates at true game scale |
| `compare_real.py` | candidates *next to the real art pack*, with numbers |
| `compare_sets.py` | several finished sets against each other, one row each |
| `measure_blocks.py` | how much real detail a render contains (block-size analysis) |
| `sweep_finish.py` | grid over the finishing parameters, laid out to pick by eye |
| `run.sh` | runs any of the above with Pillow on the path |

## 3. Where the output goes, and why not `data/sprites/`

Everything lands in `sprite_preview/comfy_out/`. **Nothing is ever written into
`data/sprites/` by these tools.** That is deliberate and is the rule from
`docs/asset-generation-design.md` §1.2: the generated layer must never feed back
into the art pack it is trying to emulate, or the next generation is trained on
its own output. Promotion into the game is a separate, human decision:

```bash
cp sprite_preview/comfy_out/final/well_0.png data/sprites/well.png
```

## 3.5 The checkpoint is the biggest lever, and it is measured

The original setup was `v1-5-pruned-emaonly` (a general-purpose photographic base)
plus the `PixelArtRedmond15V` LoRA. That is the wrong shape for this job, and
swapping the checkpoint is worth more than every prompt and sampling change
combined. Measured on the identical 11 subjects, seed 21000, 512px, cfg 7, 28
steps, `--style v2` for both:

| | base + LoRA | **AziibPixelMix** |
|---|---|---|
| accepted by the finisher | 14/24 (58%) | **20/22 (91%)** |
| subject as share of sprite | 24–65% | **52–84%** |
| outline / pixel structure | fragmented, blocky | clean, hand-shaded |
| palette vs the art pack | wrong (garish, over-saturated) | **matches** (warm, earthy) |
| complex props (cart, well, trough) | abstract confetti | legible objects |

Install it (no Civitai account needed — the API serves the file unauthenticated):

```bash
cd ~/comfyui-data/models/checkpoints
curl -sL -o aziibpixelmix_v10.safetensors \
  "https://civitai.com/api/download/models/220049?fileId=169602"
# expected sha256:
# 0a1a9d0661845ce4d58b6ed1c8dd14dff2c67ed756bf0b8007f9030abf343498
```

ComfyUI picks up a new checkpoint **without a restart** — it re-scans on
`/object_info`, verified.

### What the new checkpoint changed, and what it did not

- **Fixed:** style authenticity, outline weight, palette, and the ability to draw
  a complex object as a coherent whole rather than debris.
- **Still true:** higher native resolution is still the wrong direction (the 768
  finding below applies to this checkpoint family too), and image-to-image from a
  reference sprite is still useless.
- **Introduced:** a scene-painting bias, which is what `--style v3` exists to
  suppress. A checkpoint is not a drop-in replacement for a LoRA — it has its own
  failure modes and needs its own prompt template.

Also worth knowing: [AziibPixelMix's own model page](https://civitai.com/models/195730)
recommends passing the output through a pixel-art downscale/upscale step for an
authentic look. That is exactly what `finish_props.py` does, so this pipeline is
already the documented workflow rather than a substitute for it.

### Alternatives, and what was ruled out

- [PixelShaper v2](https://civarchive.com/models/129695?modelVersionId=142197),
  [Dreamshaper PixelArt](https://civitaiarchives.com/models/129879?modelVersionId=142421)
  and [M_Pixel v2/v3](https://civarchive.com/models/44960?modelVersionId=50623)
  are the same class of SD 1.5 pixel-art checkpoint. Any of them is a drop-in
  through `--checkpoint`; the tooling is checkpoint-agnostic.
- **SDXL is a bad trade on this hardware.** The 3050 benchmarks at ~0.6 it/s and
  ~1.2 images/min at 1024x1024, a base SDXL checkpoint is ~7 GB against 5.67 GiB
  of VRAM, and generating at 1024 is the direction the 768 experiment already
  proved harmful for sprite work.
- **Flux is out of reach.** A 24 GB-class model on 6 GB means GGUF/CPU offload at
  minutes per image. The `ComfyUI-GGUF` and bitsandbytes-NF4 nodes are installed
  if that ever changes.

---

## 3.6 The blur: wrong generation size and the wrong downscale filter

This one was caught by the owner looking at the images, not by the numbers, and it
is worth recording because both metrics I was tracking were blind to it.

**Symptom:** every sprite looked "very pixelated, to the point where they look
kind of blurry" — at the same time, which sounds contradictory but is exactly
right for the failure.

**Cause, measured.** `measure_blocks.py` on a 512px AziibPixelMix render reports a
2px dominant run, i.e. the model's pixel-art grid is **~half the canvas** — about
256px. Downscaled to a 32px sprite that is **8 real pixels crushed into every
output pixel**, which produces dithering mush rather than a crisp sprite. Two
contributing mistakes, both mine:

| mistake | effect |
|---|---|
| generating at 512 (the "more pixels must be better" assumption) | 8:1 downscale; detail averaged into noise |
| Lanczos downscale, chosen to "match the soft shading" of the real art pack | invents in-between colours and soft edges — the literal definition of blur on pixel art |

The 768 experiment had already shown that more canvas is worse, but I read it as
"the model fragments at 768" and missed the simpler lesson: **for a 32px target,
generate near 32px, not near 512px.**

**Fix, measured.** Same 9 props, same seed, 256px generation, NEAREST downscale:

| | 512 + Lanczos | 256 + NEAREST | 128 + NEAREST |
|---|---|---|---|
| accepted by the finisher | 16/18 | 17/18 | 18/18 |
| reads as a sprite at 1x | no — muddy | **yes** | yes, but coarse |
| subject fidelity | better (lost at 32px anyway) | good | degraded |

`quant_compare.png` and `final_scale_check.png` in `comfy_out/` are the evidence.
At 4x the 512 render still looks *more detailed* — which is precisely why this
went unnoticed: every earlier review was at 6-10x zoom, where the extra detail is
visible. **At 1x, the scale the game actually draws at, 256+NEAREST is far
crisper.** The rule that follows: *review sprites at 1x, and treat a higher-zoom
view as a debugging tool, not a quality view.*

**Palette lock is roughly neutral** here (see `lock_compare.png`) because the
pixel-art checkpoint already produces a palette close to `data/palette.json`. It
stays optional.

---

## 3.7 Do not use a vision LLM to choose sprites

Three framings were tried: a forced ranking, a ranking plus a reject flag, and a
categorical per-candidate verdict (`keep`/`usable`/`reject`) with an explicit
`any_acceptable` boolean. Two models: `typesafe/jev-router` and
`google/gemini-3.8-flash`. The tools are `sprite_eval.py` (runs a judge) and
`judge_scorecard.py` (scores it against `human_grades*.json`).

**As a chooser, it fails, and the first result was an illusion.**

| | tuned sheet (11 props) | **unseen set (seed 91500)** |
|---|---|---|
| Jev picks the owner's variant | 7/7 | **1/8** |
| Gemini | 5/7 | 0/8 |

The 7/7 was measured on the same sheet that three successive schema designs had
been iterated against. On a fresh seed neither judge reproduced it. **This is
worth stating plainly because the tuned number looked like a success: it was
overfitting to a test set I had effectively trained on.**

**As a shortlister it is weak but not worthless.** Using `keep`+`usable` as the
shortlist, Jev's shortlist contained the owner's accepted variant in **8/8**
props, cutting review from 22 to 13 sprites on the props that have an acceptable
variant. But: a *random* shortlist of the same per-prop sizes covers all 8 props
with probability **4.9%** — so p ≈ 0.05 on n=8, which is suggestive at best,
and it comes after many trials that were not corrected for.

**As a screener it fails.** The question that matters most is "is anything here
good enough", because 4 of 11 props have no usable variant. Neither judge can
answer it: on the unseen set both correctly emptied only 1 of 3 all-bad props
(wheelbarrow), and both shortlisted sprites for the all-bad `hay_bale` and
`water_trough`. Jev also rated its pick for an all-bad water trough **9/9/8/9**
on a 0–10 scale, so the scores contain no rejection threshold at all.

### Rules that came out of this, and cost render cycles to learn

- **A free-text `notes` field is not evidence.** An earlier schema carried one;
  prose read from it was then reported as the model "volunteering explanation".
  Structured output means the decision is in typed fields — quote those.
- **Do not ask for a ranking and a rejection at once.** The two conflict and the
  judge resolves it by always crowning something.
- **Include the hand-made art as a control in every run.** It is what exposed
  that both judges score references in a compressed band (Gemini: 6.0–6.1 on
  every prop) or invert the ordering outright (Jev: generated 6.45 vs reference
  5.06), and it costs nothing.
- **Validate on a seed the judge has never seen.** Any number measured on the
  sheet you tuned against is not a measurement.

### Where that leaves curation

Human judgement remains the only reliable selector. What the judge could
plausibly buy is a pre-filter that removes the obviously-dead candidates before a
person looks — worth roughly a 40% reduction in review, at the cost of trusting a
signal that currently sits at p ≈ 0.05. Given that a missed sprite is invisible
(a rejected good variant simply never gets seen), the failure mode is silent, so
the default should stay: **look at every candidate.**

## 4. What actually went wrong, measured

Three findings from the first runs. They are written down because each one cost a
render cycle and each one is counter-intuitive.

### The prompt must not ask for pixels

The first template asked for "chunky readable pixels, hard edges, limited
palette". The model obeyed literally: it painted a ~64px image and scaled it up to
fill the 512px canvas. `measure_blocks.py` on the result showed a dominant block
size of ~8px, i.e. a native grid of 64×64. Downscaling a 64×64 image to a 32×32
sprite leaves four pixels of the original per pixel of output, and the barrel was
unrecognisable.

The pixel-art look has to come from **the LoRA plus the 512→32 downscale**, never
from the prompt. `STYLE_V2` in `comfy_props.py` is the corrected template.

### A named background colour does not survive

Asking for "a flat magenta `#FF00FF` background" got a green field instead: a
large flat colour region is a thing SD 1.5 will happily replace with whatever it
prefers. The fix is not a better colour word. It is to stop caring what colour
the background is — ask only for an *empty flat background*, and **detect** the
colour afterwards. That is what `finish_props.flood_background` does, flooded from
the four corners so that interior regions of background-like colour inside the
subject are not punched out.

### A "flat" background is not flat

The detected green background spanned thousands of near-identical values around
`(50, 174, 34)`. Any equality test fails. Detection is a flood fill with a
per-channel tolerance (default 38), and the alpha matte is solved from an
*estimated* local background rather than one global colour.

### The numeric gate

`finish_props.py` refuses rather than emits:

- **background coverage < 5%** — the render is a full-frame pattern, not an
  object on a field. This is the v1-barrel failure, caught automatically.
- **no connected blob above 2% of the frame** — after isolating the largest
  region there was nothing left.
- **subject box >92% solid across >90% of the frame** — the background was never
  separated, so the "object" is the whole image.

A rejection prints a reason and writes nothing, so a bad batch cannot quietly
become a bad art pack.

### Two approaches that did not work, and how they were falsified

Both were plausible and both are measured dead ends. They are recorded so the
next person does not spend a render cycle rediscovering them.

**Higher generation resolution (768px) is worse, not better.** The hypothesis was
that 512px renders have ~8px pixel blocks, so a 32px sprite only retains ~4 real
pixels; more canvas should mean more detail. It does not. `compare_sets.py` on the
same six subjects and seeds shows 768px output fragmenting: the well became a
spray of discs, the wheelbarrow a pile of debris. SD 1.5's capacity does not scale
with canvas, so the extra pixels are spent on incoherence. **512px is the right
size for this checkpoint.**

**img2img from a real prop collapses to that prop.** The hypothesis was that
seeding from `data/sprites/anvil.png` would transfer composition and outline
weight, fixing the batch-consistency problem. Tested at denoise 0.55 and 0.40 on
six subjects: *every* subject came back an anvil. At denoise ≤0.55 the reference
dominates the text prompt completely, and above ~0.75 the reference is discarded
and it is text2img with extra steps. There is no window where it helps. `--init-image`
is kept in the tool because the effect is worth being able to reproduce, but it is
not a working strategy.

What *did* work is the unglamorous one: **text2img at 512, many variants, curate.**
Local generation is free per image, so the answer to inconsistent output is more
attempts and a trusted contact sheet, not a cleverer graph.

## 5. Honest limits

- **The checkpoint swap raised the floor; it did not finish the job.** With
  AziibPixelMix the palette, outline weight and shading genuinely match the art
  pack, and complex props are legible rather than debris — but the model still
  decorates. Barrels come back with plinths, carts with awnings, signposts with
  vines, and a minority of props are still unrecoverable. Expect to reject a
  meaningful fraction and to curate the rest by hand.
- **Do not compare these to the Gemini art and expect parity.** The existing
  `data/sprites/` props were made by a stronger model and hand-reviewed. Judge
  with `compare_real.py` / `compare_sets.py`, which put them side by side.
- **A soft contact shadow is the recurring defect.** Both checkpoints like to
  paint one even when the negative prompt forbids it, and it survives as a grey
  rim at 32px. `--alpha-cut`, `--alpha-gain` and `--erode` in `finish_props.py`
  all attack it; `sweep_finish.py` is how to pick between them.
- **Prompt templates are checkpoint-specific.** A template tuned for one
  checkpoint is not portable to another — `v2` and `v3` exist for exactly this
  reason. Budget a render cycle to re-tune whenever the checkpoint changes.
- **Only props.** A tileable terrain tile needs seam-free output, which is a
  different problem (`tools/gen_tile.py` and its `seamless tileable` prompt are
  the starting point). Character sprites need frame-to-frame consistency; a
  single still is not a sprite sheet.
- **Generated sprites are binary assets.** They cannot be diffed and do not
  compress well. Decide deliberately whether they get committed.

## 6. Tuning, by symptom

| symptom | change |
|---|---|
| unrecognisable at 32px, render looks like scaled-up blocks | you asked for "chunky pixels" in the prompt; remove it. Check with `measure_blocks.py` |
| the sprite is a *scene* (building, ground, plants) | use `--style v3`; "object"/"isolated" in the positive, scenery words in the negative |
| **looks blurry and over-pixelated at once** | you are generating at 512 and/or downscaling with Lanczos. Use `--size 256` and the default NEAREST. Verify at **1x**, not zoomed (§3.6) |
| grey rim / halo around the sprite | `finish_props.py --alpha-cut 0.3`, then `--erode 1` |
| subject too small in its sprite | the crop margin is doing its job; check `subject_px` in the output table |
| subject touching the sprite edge | raise `--margin` from 0.06 |
| background not removed at all | render has no keyable background (REJECT); reroll it |
| colours clash with the art pack | check you are on the pixel-art checkpoint, then `--lock` to quantise to `data/palette.json` |
| output looks garish and over-saturated | you are on the base checkpoint; use `--no-lora --checkpoint aziibpixelmix_v10.safetensors` |
| props all slightly different in style | keep one seed base and reroll per prop rather than per batch |
| one prop is consistently bad | the subject phrase is too vague; describe the *object*, not the noun |
