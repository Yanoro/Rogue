# Local tile generation (SD 1.5 + pixel-art LoRA)

A design-time experiment: generate a tile sprite with a local diffusion model and
find out whether the output is good enough to sit next to the hand-authored art in
`data/sprites/`.

Nothing here runs at game runtime. It writes PNGs into `data/sprites/` and,
optionally, a catalog entry into `data/tiles/`, so a generated tile is immediately
usable in the map editor.

**The question this is meant to answer is judgement, not plumbing:** does a
generated tile belong beside `tile_grass.png`? That is why the tool's main output
is a side-by-side contact sheet at true game scale, not the 512px render.

---

## 1. Setup on NixOS

On NixOS you cannot `pip install` into the system Python — it lives in the read-only
store. `buildFHSEnv` gives a normal FHS layout where a venv and pip work as
documented, while keeping a glibc the PyTorch wheel is happy with.

Save this as `~/sdx/shell.nix`:

```nix
{ pkgs ? import <nixpkgs> { config.allowUnfree = true; } }:

(pkgs.buildFHSEnv {
  name = "sdx";
  targetPkgs = pkgs: with pkgs; [
    python3 python3Packages.pip python3Packages.venv
    git curl wget cacert stdenv.cc.cc.lib zlib libGL
  ];
  profile = ''
    # The NixOS-specific line that matters. The pip CUDA wheel bundles its own
    # CUDA runtime, but it still needs the driver, which NixOS exposes here.
    # Without this, torch.cuda.is_available() is False.
    export LD_LIBRARY_PATH=/run/opengl-driver/lib:$LD_LIBRARY_PATH
    export PIP_CACHE_DIR=$HOME/.cache/pip
  '';
  runScript = "bash";
}).env
```

Then:

```bash
nix-shell ~/sdx/shell.nix
python3 -m venv ~/sdx/venv && source ~/sdx/venv/bin/activate
pip install torch torchvision --index-url https://download.pytorch.org/whl/cu126
pip install diffusers transformers accelerate safetensors pillow
```

### Check CUDA before downloading any model

This is the go/no-go, and it is not the same as `nvidia-smi` — that can fail its
NVML init while CUDA works perfectly. On this machine it does.

```bash
python -c "import torch; print(torch.__version__, torch.cuda.is_available(), torch.cuda.get_device_name(0))"
python -c "import torch; p=torch.cuda.get_device_properties(0); print(round(p.total_memory/2**30,1),'GB')"
```

Want `True` and an 8 GB `NVIDIA GeForce RTX 3050`. If it is `False`, stop here and
fix the driver before downloading weights.

An alternative that avoids pip entirely is `nix-shell -p
python3Packages.torchWithCuda`. nixpkgs 25.11 has torch 2.9.1 with CUDA, and
`allowUnfree = true` is already set in this machine's config. The catch is that
`torchWithCuda` is a very large closure: if it is not already in a binary cache you
have configured, Nix will try to **build PyTorch from source**, which is hours. Test
with `nix-shell -p python3Packages.torchWithCuda --run 'python -c "import torch; print(torch.cuda.is_available())"'`
and Ctrl-C if it starts compiling.

## 2. Models

| what | where it goes | size |
|---|---|---|
| SD 1.5 base | any path; pass with `--model` | ~4 GB |
| pixel-art LoRA | any path; pass with `--lora` | ~100 MB |

`--model` accepts a HuggingFace repo id or a local `.safetensors`. If
`runwayml/stable-diffusion-v1-5` gives a 404 (that repo has been in flux), use a
local SD 1.5 checkpoint and pass its path — ComfyUI's `models/checkpoints/` or any
Civitai SD 1.5 download works.

For the LoRA, search Civitai for SD 1.5 pixel-art LoRAs and open the model page.
**Copy its trigger words.** Omitting them is the single most common reason a LoRA
appears to do nothing — the weights alone change very little.

## 3. Running it

```bash
# no model needed: verifies paths, quantisation, naming and the contact sheet
python3 tools/gen_tile.py "mossy stone" --dry-run

# the real thing
python3 tools/gen_tile.py "grass" \
  --model /path/to/sd15.safetensors \
  --lora /path/to/pixel-art-lora.safetensors \
  --trigger "pixel art, <lora trigger words>" \
  --count 8 --emit-tile-json
```

Writes:

- `data/sprites/tile_<name>_0..7.png` — the pixelized tiles, 32×32
- `data/sprites/tile_<name>.png` — the first one, under the convention name
- `data/sprites/tile_<name>_sheet.png` — **the output that matters**, generated row
  over the existing game sprites at true game scale
- `data/tiles/<name>.json` — with `--emit-tile-json`, so the tile is selectable
- `--raw-dir` (default `/tmp/air_tile_raw`) — the unprocessed 512px renders

Keep the raw renders. When a result looks wrong, the first question is always
"is it the model or the downscale?", and they answer it.

## 4. Judging the result

Look at it **in the game**, never in an image viewer. A 512px texture can look
excellent on its own and read as mush at 32px in a field.

1. Paint a **10×10 patch** in the map editor. A single tile will not show a seam;
   a patch will.
2. Put it next to the real tiles in the same field and compare at the same zoom.
3. Ask the two questions that decide the whole approach:
   - **Does it repeat without a visible grid?** Seams are the most common failure
     and are invisible until you tile it.
   - **Does it look like the same artist made it as `tile_grass.png`?**

If grass passes both, walls and props will follow. If grass fails both, no prompt
tuning fixes the pipeline, and the Pixel Crawler pack in `data/tilesets/` is
already sitting there.

## 5. What to tune when it looks wrong

| symptom | change |
|---|---|
| blurry, not pixel art | you quantised after downscaling, or used BILINEAR. Quantise at 512 first, then `NEAREST` |
| seams in a field | prompt `seamless tileable`; try generating at 512 and mirror-wrapping before the downscale |
| LoRA seems absent | trigger words missing from `--trigger`, or `--lora-scale` too low. Try 1.0 |
| oversaturated, garish | lower `--cfg` (7 → 5), lower `--lora-scale` |
| looks photographic | strengthen the negative prompt; raise `--lora-scale` |
| too much fine detail to survive 32px | simplify the prompt; fewer `--colors` |

## 6. Honest limits

- **You will reject most output.** Expect 1 usable tile in 5–10. At
  ~10–30 s each on an 8 GB RTX 3050 that is a few minutes per usable tile, which is
  fine for a design-time tool and unusable for anything live.
- **This cannot do the 4x47 blob-tileset thing** (terrain that adapts to its
  neighbours). That multiplies asset count per material enormously and is the wrong
  trade here.
- **Generated sprites are binary assets.** They cannot be diffed and do not
  compress well; `data/sprites/` already has untracked files. Decide deliberately
  whether generated art gets committed.
- **ComfyUI is optional.** This script deliberately avoids it: the graph is built
  for chaining pipelines, and this task needs one fixed pipeline run many times
  with the post-processing attached. If you prefer the GUI, the equivalent graph is
  `Load Checkpoint → Load LoRA → CLIP Text Encode (x2) → KSampler ← Empty Latent →
  VAE Decode → Save Image`, with size and batch on `Empty Latent Image`. You still
  do the quantise and downscale by hand afterwards.

---

## 7. The API path, and the live half

`gen_tile_art.py` is the *other* backend behind the same idea, and the one the agent
path uses: one OpenRouter image request (Gemini) for **a batch of tiles at once**,
then cut, palette-lock and downscale into the generated layer. It exists because
this document's local path is blocked on the host CUDA fault, and because a tile an
agent invents at runtime cannot wait for a manual run.

The two differ in where the output goes, not in what they produce:

| | `gen_tile.py` (section 1) | `gen_tile_art.py` |
|---|---|---|
| backend | local SD 1.5 + LoRA | OpenRouter sheet API |
| output | `data/sprites/` — the hand-authored set | `packs/generated/<pack>/sprites/` — the generated layer |
| driver | a human, at design time | the tile designer's creations, plus the art gaps |
| review | a contact sheet, then commit by hand | the owner reviews the cut sprites |

**Cost is per request, not per tile** — 1,290 image tokens / $0.039 whether the sheet
holds one cell or four — so the script always sends exactly one request for every
name it is given. `--dry-run` costs nothing, `--sheet` re-cuts an already-paid sheet
for free, and the raw sheet is kept. See `docs/tile-art-wiring-design.md`.

The cut tiles are placed and hot-swapped by the game itself:

```
AIRogue --tile-art <name>                          # where would it go, what wins
AIRogue --tile-art <name> --tile-art-sprite <png>  # place it, check both contracts
```

`tests/stub_tile_art.py` is the same contract with flat colours and no network, so the
whole live path can be exercised for free. It writes the stage files the pipeline
panels read (`tile_art_sheet.prompt.txt`, `tile_art_sheet.png`, and
`cell_<index>_{crop,trim,locked}.png`) as well as the final sprites — the panels are
part of the path, and a stub that skipped them could not show whether they had anything
to draw.

### Naming: the engine decides the filename, the tool uses it

`--from-worklist` passes each cell's **kind** and **convention id** to the generator (via
`--from-cells <normalized-worklist.json>`), and the tool writes `<conventionId>.png`. The
id is *not* re-derived here: a tile is `tile_<slug>` and an object is its bare template key,
and a second implementation of that rule in Python is how object sprites came to be written
as `tile_apple_sapling.png` while the engine looked for `apple_sapling.png` -- a paid-for
sprite that was never loaded, with nothing reporting it.

`slugify` survives only as a fallback for a worklist written by an older build that recorded
nothing but a name list. A bare `NAME` on the command line is treated as a tile, because
that is what every old invocation meant; a caller that wants objects passes a worklist.

The kind also decides the prompt's *shape*: a tile is asked for as a full-bleed surface and
an object as one complete thing centred in its cell. That was a word-list guess
(`SURFACE_WORDS`) and is now the recorded kind, verified on a mixed sheet: an object called
"stone wall" gets the object framing and a tile called "archway of bells" gets the surface
one, which the old heuristic had backwards in both directions.

### Objects are cut out; tiles are not

The two kinds take different cut paths, and the difference is not cosmetic. A **tile** must
fill its cell, so the pipeline washes the gutter into the surrounding art and produces an
opaque, edge-to-edge sprite. An **object** sits *on* the ground, so an opaque background means
it covers whatever it is placed on — measured on a real generation, 1024 of 1024 pixels
opaque.

`cut_object_cell` therefore keys the background out (the prompt asks for flat magenta, with
the subject not touching any edge), trims to what remains, pads back to a centred square so the
object keeps its proportions, resizes NEAREST, and palette-locks **with the alpha lifted out
first** — `-remap` knows nothing about transparency and would repaint the keyed area with the
nearest palette colour.

The key is only as good as the test for "is this magenta". That test used to be absolute channel
floors (`R >= 140`, `B >= 140`, `G <= 120`), tuned to one observed sample `srgb(205,50,141)` whose
blue cleared the floor by **one** unit. The next real generation came back `srgb(215,52,135)` —
blue five *under* it — so the key was skipped, the uncut cell was palette-locked, and the object
arrived with an opaque red background. `looks_like_magenta` now tests the relationship instead
(`r > 110 and b > 100 and g * 2 < min(r, b)`), and `--cut-check` carries both observed magentas
plus the textures that must not match, so a return to a channel floor fails the test.

Even a correct key only removes a background that is actually there. `--draw-probe <templateKey>`
in the game draws a frame with the object placed on a mapped tile and reads the pixels back: if a
transparent sprite pixel differs from the same pixel without the object, something is painted
behind it. Use it after generating an object; it costs nothing.

If fewer than 2% of pixels survive the key, the subject is too close in colour to its
background to separate; the uncut cell is used and the reason is printed, because a fully
transparent sprite is worse than an uncut one — it hides the object's glyph fallback.

### The cut stage, and the "gutter" that is not there

`python3 tools/gen_tile_art.py --cut-check` self-tests the colour logic that decides how
much of a cell survives. It is free and needs no files.

Both `frame_inset` and `wash_gutter` identify the gutter as "the cell's own edge colour".
That is only valid when a gutter exists. On a **one-cell sheet** the model paints the
surface edge to edge — which is exactly what the cell description asks for — so the edge
colour is a real texture colour. On the dark brick floor this destroyed, the edge was
`srgb(66,46,38)`, and ±56/channel of that covers most of the brickwork: measured,
`wash_gutter` replaced **580,380 of ~795,000 pixels (73%)**, and the finished sprite was
a flat reddish-brown blob with the detail of the initial drafts gone.

The fix is in two places:

- the edge colour now has to pass `looks_like_magenta` before either stage treats it as
  a gutter, so a full-bleed tile is passed through untouched;
- the prompt only asks for a magenta gutter when the sheet has more than one cell, since
  a gutter's whole purpose is separating neighbouring cells. Asking a one-cell sheet for
  both "fill the cell edge to edge" and "leave a magenta gap around it" is a
  contradiction the model resolves by ignoring the gap.

A one-cell sheet therefore has no gutter and needs no cut; a multi-cell sheet still gets
one and still gets the fringe removed.

**Run it against a copy of the pack, not the real one.** The stub writes wherever the
worklist's `spritesDirectory` points, which comes from `art.pack`, so a test run with the
real settings overwrites real generated sprites (`packs/generated/<pack>/sprites/` is
tracked). The harnesses point `art.pack`/`art.generated` at `/tmp`; see the settings
example in `SETTINGS.md` §`art.generation`.
