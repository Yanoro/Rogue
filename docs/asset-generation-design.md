# Art packs and data packs: re-skinning a world, and growing art to fit what the agents build

**Status: design note. Nothing in here is implemented except where it cites existing tooling.**
Written 2026-10-15, revised 2026-10-15 for the art pack / data pack split. It answers two questions:

1. *When an agent builds something the art pack has no art for, how does the art come to exist, in
   the art pack's own style?*
2. *What has to be true for an art pack and a data pack to be swapped independently?*

Read `blueprint-design.md` first — the designer agent that emits the structures this document has to
dress. Read `TileRegistry.h` for the tile catalog that is the world's material vocabulary. Read
`tools/README.md` for the design-time tile generator and the measurements it already took.

---

## 1. Two kinds of pack

The word "pack" was doing two jobs, and separating them is the load-bearing decision in this
document.

| | **data pack** | **art pack** |
|---|---|---|
| owns | gameplay-critical definitions: materials, objects, items, recipes, stats, skills, races, blueprints | the art style: sprites, palette, glyph *appearance*, fonts, chat colours |
| changes what | an agent can name, and what the world *does* | what the world *looks like* |
| example | a village economy; a cyberpunk workshop set | high fantasy; dark fantasy; Pixel Crawler |
| file today | `data/{tiles,objects,items,recipes,stats,skills,races,blueprints}/` | `data/sprites/` (+ colours scattered in the data files) |

The two are **orthogonal**, joined at exactly one place: **the name.**

That join key is not a design to be invented — it already exists everywhere:

- `TileRegistry::Get(name)` resolves a material by string
- a blueprint says `"paint": "stone wall"` — a name, not an id or a pointer
- `SpritePaths.cpp` resolves the convention path `data/sprites/tile_<slugified name>.png`
- `ObjectFactory` keys templates by id and falls back to `data/sprites/<id>.png`
- `GetSurroundingsRadar` prints a legend mapping glyph -> `tile->name`
  (`include/AgentActions.h:550`)

So an art pack is **a partial map from name to appearance**, and the data pack is **the set of names
and what they mean**. The split is not new machinery. It is un-fusing two halves of a record that are
currently stored in the same file — `data/tiles/wooden_wall.json` says both `"blocksTile": true` and
`"characterColor": [200,200,200,255]`.

### 1.1 What the split buys

The two axes recombine, and each combination is meaningful:

- **one data pack, many art packs** — the same village economy rendered high fantasy, then dark
  fantasy. Gameplay identical, world unrecognisable.
- **one art pack, many data packs** — a village economy and a cyberpunk workshop set both drawn by
  the same hand, so two mechanically different worlds read as one coherent product.
- **the gap-filling pipeline is what makes the second case practical.** See §3 — this is the part
  that is not obvious, and it is why this document still exists after the split.

---

## 2. What separates cleanly, and the one thing that does not

Going field by field through the definitions, most of it falls on one side without argument:

| field | today lives in | belongs to | why |
|---|---|---|---|
| `name` | tiles, objects, races | **data** | the join key *and* the legend value in `[SURROUNDINGS]` |
| `description` | objects | **data** | the LLM reads it; it is meaning, not pixels |
| `components`, `capacity` | objects | **data** | behaviour (`Interactable`, `Obstacle`, `Storage`, `Workstation`) |
| `blocksTile`, `hitboxSize` | tiles | **data** | collision, walkability, placement validation |
| `slots`, `accepts` | races | **data** | equipment layout |
| `sprite` | tiles, objects, races | **art** | a path; nothing reads it but the renderer |
| `art` (multi-line grid), `palette` | objects, races | **art** | render-only, see below |
| `characterColor`, `backgroundColor`, `nameTagColor` | tiles, objects | **art** | render-only |
| font, chat colours | `Defaults.h`, `render.fontPath` | **art** | legibility and mood, not mechanics |

### 2.1 The exception: the single ASCII glyph is not art

`character` — the one-character field — **must stay in the data pack**, and this is the finding worth
reading carefully.

It is not a cosmetic fallback. It is **the encoding the agents perceive the world through.**
`GetSurroundingsRadar` (`include/AgentActions.h:524-575`) builds a map of glyph -> name and prints a
character grid plus its legend into the prompt:

```cpp
tileChar = tile->ascii->ch;
// Terrain is what placement validation checks against ("tilled soil", "path"),
// so the grid has to name it.
legend[tileChar] = tile->name;
...
result += std::string(1, pair.first) + " = " + pair.second + "\n";
```

So the glyph is a **symbol table key** with a correctness invariant. `extract_sheet.py` states it
outright: *"Characters are deliberately unique because [SURROUNDINGS] prints a glyph grid and a
collision makes that grid unreadable."*

Two concrete consequences:

- **An art pack that changed `character` would change agent behaviour.** Not rendering — reasoning.
  Two art packs over one data pack would produce different agent transcripts from the same seed.
  That contradicts the split's whole point.
- **An art pack that collided two glyphs would corrupt `[SURROUNDINGS]`** into an ambiguous grid,
  which is a gameplay bug caused by a reskin.

The multi-line `art` grid and `palette` are different: nothing reads them for prompts. Confirmed —
their only consumers are the world renderer (`GameECS.cpp:405`), the storage window
(`StorageWindow.cpp:166`) and the editor's template palette (`DebugWindows.cpp:1140`), all through
`AsciiArtDraw.hpp`. **`art`/`palette` are art; `character` is data.**

The rule that follows: **the art pack owns how a thing is drawn; the data pack owns how a thing is
identified.** They are different fields, and the split must respect that they are different *kinds*
of field, not just different files.

### 2.2 Resolution is genuinely free

`DrawSpriteFitted` (`include/SpriteDraw.hpp:55`) fits any texture into the entity's `DrawAscii` box via
`SpriteFitRect`, and `TileDef` states the logical size in `hitboxSize` which `Map` scales to that
map's tile size.

So an art pack can ship 16x16, 32x32 or 48x48 sprites without touching a single gameplay value, and a
map with different `tileWidth`/`tileHeight` still works. **Tile size is not a coupling point**, which
is better than this document originally assumed.

---

## 3. The consequence nobody wants to hear: art packs are systematically incomplete

If a data pack defines 200 materials and an art pack covers 60 of them, then every combination of
that data pack with that art pack has **140 missing sprites — by construction, not by oversight.**

This is not a defect to be fixed by authoring more art. It is arithmetic: `N` data packs x `M` art
packs is `N*M` pairings, and no authoring effort keeps `M` current with every `N`.

What saves it is that **the engine already treats a missing sprite as normal**, and that is the
single most important existing property this whole design leans on:

- `SpritePaths.cpp` resolves explicit path -> convention path -> **nothing**, and the entity falls
  back to its ASCII look with a warning: *"sprite ... does not exist; using the ASCII look instead."*
- `Sprite` only exists as a component when the image loaded (`Components.h:119-135`), so "no sprite"
  is not a special case in the renderer.
- The `character` glyph is in the **data** pack (§2.1), so a mixed pack always has a legible
  fallback and a working `[SURROUNDINGS]` regardless of which art pack is loaded.

So recombination is **safe but visibly partial** — half sprites, half glyphs. Which is exactly the
problem the generation pipeline solves, and it reframes what the pipeline is for:

> **The pipeline is not a convenience. It is what makes independently swappable art packs viable at
> all.** An art pack without it is frozen at whatever coverage it shipped with, and every new data
> pack degrades it further.

### 3.1 The LoRA is the art pack's style model

This is the corrected framing of the original idea. A LoRA is not trained "on a data pack", and it is
not a global model — it is **the art pack's own style model**, and its job is to extend that art
pack's coverage over *any* data pack's vocabulary.

- Owned by, versioned with, and loaded from the art pack (§8.3).
- Trained on the **art pack's** source art (§8.1) — for a Pixel Crawler art pack, that is the 26
  atlas-grade pages in `data/tilesets/`, not the data pack's 32x32 crops.
- Applied to close gaps between that art pack and whichever data pack is loaded.

A LoRA does not create a style, it **amplifies** the consistency of its training set (§8). An
art pack that is already incoherent produces a confident, reproducible incoherence.

### 3.2 The rule that keeps the split honest

**An art pack must never gate the data pack's vocabulary.**

The designer agent's `%MATERIALS%` block is built from the **data** pack's tile catalog. If it were
instead built from the *intersection* of the data catalog and the art pack's coverage, then swapping
from a dark fantasy art pack to a high fantasy one would change **what can be built** — contradicting
"data packs contain critical gameplay information" and silently coupling the two axes.

So: the designer designs against the data pack. Missing art is a rendering concern that the pipeline
addresses, never a design constraint. If an art pack wants to *bias* the designer's taste, that is a
prompt hint at most (§11), and it must remain advisory rather than a gate.

---

## 4. The rule that keeps the art pipeline from corrupting the data pack

A specific hazard, because the current tooling already does the wrong thing.

`gen_tile.py --emit-tile-json` writes **`data/tiles/<name>.json`** — the art generator currently
writes into the *data* catalog. Under this split an art pipeline must **not** create or edit data
definitions, for two reasons:

1. **It would violate §3.2** — art generation would be adding to the vocabulary the designer designs
   against.
2. **It would renumber tiles.** `todo`'s "Map/tile id bug" documents that a map stores terrain as
   `"positions"` holding integer **indices** into `tileInfo`, and `Map::uniqueTiles` order *is* tile
   identity. `Map::CreateTile` is safe today only because it appends to both containers in lockstep.
   An art pack injecting definitions would add entries to one and change what an existing index
   means — **every wall becomes floor.**

So the boundary is:

| the pipeline may write | the pipeline may never write |
|---|---|
| `sprites/<name>.png` in the art pack | `tiles/*.json` |
| the art pack's `palette` | `objects/*.json` `components` |
| the art pack's `pack.json` style block | any `content.*` definition it did not create deliberately |

When a gap is genuinely a *new material* rather than a missing sprite for an existing one, that is a
**data pack change**, made deliberately and reviewed, and it must never be an automatic side effect of
generating a PNG.

---

## 5. What already exists

Do not rebuild these. Each is a working stage of the pipeline, built by hand for the church.

| file | stage | what it does | evidence it works |
|---|---|---|---|
| `sprite_preview/gen_object_sheet.py` | source | prompts `google/gemini-2.5-flash-image` for **one** 1024px sheet of many objects, with `data/sprites/farmer.png` passed as a **style reference image** | produced the 19-object object set |
| `tools/sprites/extract_sheet.py` | cut | slices a sheet into 32x32 RGBA sprites: edge-seeded flood-fill background removal (JPEG-safe), crop-to-content, `NEAREST` downscale, centre on a 32x32 canvas | the 22 church sprites in `data/sprites/` |
| `tools/sprites/import_church.py` | catalog | writes object/tile templates with ASCII glyph fallbacks and palette entries | the 38 object and 20 tile definitions |
| `tools/gen_tile.py` | source (local) | SD 1.5 + a pixel-art LoRA via diffusers, at 512px, quantise-then-downscale, contact sheet at true game scale | the design-time tile experiment |
| `tools/workflows/pixelart_sd15.json` | source (ComfyUI) | the equivalent fixed graph, if the GUI is preferred | — |

Measured facts from `tools/README.md` that govern the design and are not re-litigated here:

- **1 usable tile in 5–10**, at **10–30 s each** on the 8 GB RTX 3050. Fine for design time,
  unusable for anything live (§10.1).
- **Quantise at 512, then `NEAREST` downscale.** Quantising *after* downscaling produces blur.
- One image request costs the same regardless of subject count (**1,290 image tokens per 1024px
  output** — measured), so **batching many assets into one sheet is a ~19x saving** and, more
  importantly, makes the batch internally consistent by construction.

Note that `import_church.py` writes *both* halves of the record — it is the clearest example of the
conflation §1 describes, and splitting it in two is part of the work.

---

## 6. The gap: what actually triggers generation

Agents never "need a new asset" in the abstract. They name something specific whose **art** does not
resolve, and that is enumerable.

A blueprint references:

- **materials** by string — `"op": { "paint": "stone wall" }`, resolved against the data pack's tile
  catalog (`TileRegistry::Get` returns `nullptr` for an unknown name by contract)
- **objects** by string — `"op": { "place": "altar" }`, resolved against the data pack's
  `objects/*.json`

and separately, the **art pack** is asked for an appearance for each. There are therefore **two
distinct kinds of miss**, and conflating them is the mistake §4 exists to prevent:

| miss | means | action |
|---|---|---|
| **vocabulary miss** | no data definition exists | a deliberate data pack change; never automatic |
| **coverage miss** | the definition exists; the art pack has no sprite for it | generate art, in the art pack's style |

The gap log exists to make the **coverage** misses visible and countable. `ResourceManager` already
exposes the predicate that finds them — `IsKnownMissing` is public (`include/ResourceManager.h`), and
`SpritePaths.cpp` already distinguishes "authored but absent" from "convention probe that was always
expected to fail."

Output: an append-only `gap.log` plus a deduplicated `gap.json`, **stored in the art pack** (§8.3) —
because the coverage it describes is a property of the art pack against one data pack, not of either
alone.

---

## 7. The pipeline

Four stages, each with a working ancestor. The change is that they become one command over an art
pack instead of four scripts run by hand.

```
  gap.json                 one worklist, from §6
      |
      v
  [ SOURCE ]  ------------> sheet PNG (1024px) + raw 512px renders
      |         LoRA or API, batched, style-conditioned
      v
  [ CUT ]     ------------> 32x32 RGBA PNGs
      |         flood-fill bg, crop-to-content, NEAREST downscale
      v
  [ PALETTE ] ------------> 32x32 PNGs snapped to the art pack palette
      |         deterministic, no ML, cheap
      v
  [ CATALOG ] ------------> art pack only: sprites/, palette, pack.json
                NEVER data definitions -- see §4
```

### 7.1 SOURCE

Two interchangeable backends behind one interface. This is the seam that keeps the LoRA decision
reversible:

| backend | how | when |
|---|---|---|
| **sheet API** | `gen_object_sheet.py` as it stands: one 1024px sheet, art pack sprites passed as reference images | default; cheapest, fastest, no training, consistent by construction |
| **local LoRA** | `gen_tile.py`'s diffusers path with the art pack's adapter loaded | when offline/deterministic reproduction matters, or the art pack must not depend on an API |

Both take the same inputs — the gap list, the art pack's style block — and both emit a sheet plus raw
renders. Raw renders are kept, always (§11.2).

**Batch, never one-at-a-time.** One request per asset costs the same as one request for nineteen and
loses the internal consistency that comes from a single generation. The gap log is accumulated
precisely so it can be drained in batches.

### 7.2 CUT

`extract_sheet.py` generalised. Its two hard-won details are load-bearing:

- **Edge-seeded flood fill, not colour replacement.** The sheet is JPEG-ish, so the background is not
  one colour and bleeds around grid lines; a global replacement misses pixels while a wide fuzz eats
  real magenta inside a sprite (stained glass, the red carpet). Only background *connected to the
  outside* is removed.
- **Crop to content before downscaling.** A tall thin sprite like the cross otherwise shrinks to fit
  padding and loses its pixels.

Its weakness is a hand-maintained `NAMES` dict for the cell-to-name mapping — see §11.1.

### 7.3 PALETTE — the cheap stage that does the most work

Snap every generated sprite to a palette derived from the art pack. A LoRA is probabilistic and will
drift; palette snapping is deterministic and cannot. Colour harmony is the single strongest "same
artist" signal at 32px — far stronger than outline weight or shading idiom, which are hard to see at
that size anyway.

Palette derivation, in order of preference:

1. **The art pack's published palette**, if it ships one. Extract once from the pack's atlases with
   median-cut to a fixed size (32–48 colours is right for this art).
2. **Median-cut over the art pack's own sprites**, excluding transparent pixels, written to the art
   pack's `palette.json` as an explicit list, so it is reviewable and versioned.
3. **The `palette` entries already in the definitions**, which `import_church.py` writes today.

Snapping is nearest-neighbour in a perceptual space (OKLab is worth the few lines over RGB), applied
at 512px **before** the downscale, in the same position in the chain as quantisation.

The payoff: a weak LoRA with palette lock still reads as belonging to the set; a strong LoRA without
it can still look imported. If only one of the two is built, build this one.

### 7.4 CATALOG — and its hard boundary

`import_church.py` generalised, with §4's boundary enforced in code. For each gap the pipeline may
write **only** into the art pack:

- `sprites/<name>.png`
- a `palette` entry
- an entry recording which data-pack name the sprite answers to

It may **not** write `components`, `blocksTile`, `description`, `slots` or `character`. Those are data
(§2). A generator that infers `Interactable` vs `Obstacle` from a picture is guessing at mechanics,
and generated art is cheap while generated mechanics are not.

**Nothing enters the art pack unreviewed.** Unreviewed art is drawn to the player and to the designer's
editor immediately, and a bad sprite in the set is a permanent invitation to the next generation run
to imitate it (§11.4).

---

## 8. Training the art pack's LoRA

### 8.1 Dataset

Prepared from the **art pack's own source art**, not from the data pack's `data/sprites/`. For a Pixel
Crawler art pack, measured from `data/tilesets/Pixel Crawler - Free Pack/`:

| what | count | note |
|---|---|---|
| PNGs total | 289 | |
| atlas-grade (both dims >= 400px) | **26** | the aggregated sheets and tileset pages |
| at least one dim >= 256px | **199** | buckets down cleanly for SD 1.5 |
| small images (<= 128px wide) | 74 | animation frames; weak as style |

- **include** the 26 atlas-grade pages whole, and the other images measuring at least 256px on one
  axis (199 files clear that threshold)
- **exclude** the animation strips — 74 files are 128px wide or narrower, most only 32–48px tall.
  They teach motion frames, not style.
- **do not** include `data/sprites/`'s 32x32 crops: derived, mixed-origin, and below the resolution
  the base model works at

The decisive fact is the **atlas pages**. `Furniture.png` (800x864), `Wall_Tiles.png` (400x400),
`Floors_Tiles.png` (400x416), `Resources.png` (400x400), the five `Structures/Buildings/` pages,
`Icons/Resources.png` — large, coherent, multi-subject pages drawn in one hand. Exactly what a style
LoRA wants, and no upscaling needed.

Rough target for a style adapter is 30–100 images. The 26 large pages plus a selection of the medium
ones lands in range. Augmenting a set this small is *not* recommended: flips break asymmetric objects,
and crops of crops teach nothing.

### 8.2 Captioning: one trigger, no content

For a **style** adapter the captions should be maximally boring. Tag every image with one unique
trigger token and nothing else:

```
pxcrawler_pixel_art
```

A single token shared by every image leaves the model no content words to bind appearance to, so the
trigger collects *style* — palette, outline weight, shading idiom, dithering. Captioning the content
("a stone wall, a wooden bench") instead teaches the trigger to mean "many unrelated things" and
dilutes exactly what is wanted. Content comes from the prompt at generation time; style comes from the
trigger.

### 8.3 Configuration

Starting values, in the shape `kohya_ss/sd-scripts` `train_network.py` expects. **Community defaults
for pixel-art style adapters, not measured on this machine** — a first run to be tuned, judged by
§8.4, not by loss.

```
--network_module=networks.lora
--network_dim=32 --network_alpha=16     # dim 16/alpha 8 if the style over-fits
--network_train_unet_only               # style lives in the UNet; leave the text encoder alone
--learning_rate=1e-4
--optimizer_type=AdamW8bit
--lr_scheduler=cosine_with_restarts --lr_scheduler_num_cycles=2
--resolution=512,512
--enable_bucket --min_bucket_reso=256 --max_bucket_reso=768
--batch_size=1 --gradient_accumulation_steps=4
--mixed_precision=fp16 --cache_latents
--max_train_epochs=10 --save_every_n_epochs=2
```

The 8 GB card is the binding constraint; `--cache_latents`, `AdamW8bit` and batch 1 with accumulation
are what make it fit. Aspect-ratio bucketing is what lets the 400x416 and 896x640 pages train without
being squashed into squares.

The diffusers equivalent is `train_dreambooth_lora_sd15_advanced.py`. The FHS environment in
`tools/README.md` §1 is reusable for training as-is — the same `buildFHSEnv` shell, the same
`LD_LIBRARY_PATH` line. Training does not need the game's `nix-shell`.

**The adapter lives in the art pack, never in a global models directory**, because it *is* the art
pack's style:

```json
{
  "id": "pixel_crawler",
  "name": "Pixel Crawler",
  "lora": "lora/pxcrawler.safetensors",
  "trigger": "pxcrawler_pixel_art",
  "loraScale": 0.8,
  "negative": "blurry, smooth gradient, antialiasing, 3d render, photograph",
  "references": ["sprites/farmer.png"]
}
```

`loraScale` around 0.8 is the starting point from `gen_tile.py`'s tuning table; too high destroys
texture. `references` feeds the sheet-API backend, so both backends read the same block and swapping
between them is a config change rather than a code change.

### 8.4 Judging the adapter, in this order

1. **Contact sheet at true game scale**, as `gen_tile.py` already produces. Never judge at 512px.
2. **A 10x10 painted patch** in the map editor. A single tile cannot show a seam; a patch will.
3. **Side by side with the art pack's real sprites in the same field, at the same zoom.**
4. Only then: does it look like the same artist made it?

Good enough means a generated `altar` can sit next to the art pack's existing `altar.png` and the
difference is not the first thing you see. If it never gets there, the sheet-API backend already
passes that bar and is where the time should have gone.

**Judge the art pack against a real data pack, not in isolation.** The failure mode this catches is
one the channel split newly creates: an adapter that looks fine on its own pages but produces sprites
that clash with the *data pack's* glyph fallbacks around them. A field that is half sprites and half
ASCII is the normal recombined state (§3), so that mix is what must look deliberate.

---

## 9. The two roots on disk

### 9.1 It is already nearly free

`GameSettings::ContentSection` (`include/Settings.h`) is already a set of path strings read from
`settings.json` at launch. The header says outright that the file holds *"where the content registries
live"*:

```cpp
std::string objects    = "data/objects";
std::string recipes    = "data/recipes";
std::string stats      = "data/stats";
std::string skills     = "data/skills";
std::string items      = "data/items";
std::string races      = "data/races";
std::string tiles      = "data/tiles";
std::string blueprints = "data/blueprints";
```

and these are **wired, not merely declared**: `src/GameECS.cpp:1920-1968` passes each into its
registry, and `src/Settings.cpp:265-273` populates them from the file. The prompts are externalised on
the same principle — `designer.promptFile` -> `data/blueprints/DESIGNER.md` (`src/Game.cpp:638`), and
`ai.npcPromptFile` for the NPC prompt (`src/Game.cpp:197`). `src/main.cpp` already parses
`--settings <file.json>`, with `settings.local.json` merged on top by an RFC 7396 patch.

So **a data pack is already almost expressible as a settings file today**:

| thing | today | status |
|---|---|---|
| objects, recipes, stats, skills, items, races, tiles, blueprints | `settings.json` `content.*` -> `src/GameECS.cpp:1920-1968` | already wired ✅ |
| designer prompt | `designer.promptFile` -> `data/blueprints/DESIGNER.md` | already data ✅ |
| NPC prompt | `ai.npcPromptFile` (empty = compiled-in) | already externalisable ✅ |
| map | `map.file`, plus the `argv[1]` override | already per-launch ✅ |
| **sprite convention path** | `inline constexpr const char *SPRITE_DIRECTORY = "data/sprites/"` (`include/SpritePaths.hpp:12`) | **compile-time, and the only blocker** ❌ |

### 9.2 The one blocker is one string

Give `SpritePaths`' resolution the art pack's sprite root — a parameter, or a new `art.pack` setting —
instead of reading a `constexpr`, and both pack kinds exist. One string, one function signature.

That is a far smaller change than "build a content-root concept", which is how `airogue-direction`'s
first open question frames it. The registries were already built for this; only the sprite half was
missed — which is, in hindsight, exactly the half that the art pack split needs.

### 9.3 Proposed layout

```
packs/
  data/
    village/                 # the current data/ minus sprites
      settings.json          # content.* pointed at this directory
      tiles/  objects/  items/  recipes/  stats/  skills/  races/  blueprints/
      prompts/
        designer.md          # from data/blueprints/DESIGNER.md
        npc.md               # from DEFAULT_NPC_PROMPT
  art/
    pixel_crawler/
      pack.json              # style block (§8.3) + palette
      sprites/               # 57 PNGs, 32x32
      palette.json           # derived once, reviewable, versioned
      lora/
        pxcrawler.safetensors
        train/
          dataset/
          config.toml
      fonts/                 # optional: render.fontPath, chat colours
      gap.json               # coverage misses against whichever data pack ran
      gap.log
    dark_fantasy/
      pack.json
      sprites/               # may cover only part of the village data pack -- normal (§3)
      palette.json
      lora/
      fonts/
```

`--data-pack village --art-pack dark_fantasy` expands to `--settings packs/data/village/settings.json`
plus an `art.pack` override, added to the same `for` loop in `src/main.cpp` that already handles
`--settings`. Keeping `data/` as the default data pack and not requiring an art pack means nothing
breaks before the split exists.

The new `GameSettings` section is deliberately tiny, because everything else already exists:

```cpp
struct ArtSection {
  // Art pack directory. Empty means "sprites straight from the data pack", which
  // is today's behaviour and keeps an art-pack-less run working.
  std::string pack;
} art;
```

From `art.pack`, `sprites/`, `palette.json`, `pack.json` and `lora/` are found by convention — the
same "convention over configuration" the sprite lookup already uses.

---

## 10. Design-time by default, runtime only as a queue

### 10.1 The measurements forbid the frame loop

`tools/README.md` already answered this: **1 usable in 5–10, 10–30 s each** on this card. In the frame
loop an agent stalls mid-action for minutes to produce something probably rejected, while sharing VRAM
with whatever is serving the NPC LLM.

So the default mode is **offline**: run the game with a data pack and an art pack, collect `gap.json`,
drain it between sessions. The art pack grows; the frame rate does not notice. This is also why §6's
gap log comes first — it makes offline mode *sufficient*, because the work is captured rather than
lost.

### 10.2 If it must be live: the queue

The engine is well-shaped for this, because the fallback already exists and is already the normal
case — and after §3, a partially-covered art pack is the *expected* state rather than an edge case.

```
agent asks for an object; the art pack has no sprite for it
        |
        v
entity spawns with its data-pack glyph  <- already happens today, already warns, no crash
        |
        v
coverage gap enqueued at low priority
        |
        v
worker generates + cuts  ->  PNG lands in the art pack's sprites/
        |
        v
texture cache invalidated  ->  next frame draws the sprite
```

The glyph is not a placeholder to be tolerated; it is a legitimate rendering, and it comes from the
data pack so it is present regardless of art coverage. An agent that builds a confessional and gets a
readable `[SURROUNDINGS]` glyph immediately, with art arriving a minute later, is a coherent
experience. The game never blocks on the image model.

**The one concrete blocker**, documented in `include/ResourceManager.h`: missing paths are memoised so
that a world renderer looking up a texture per entity per frame does not re-stat the filesystem sixty
times a second. The comment states the consequence plainly — *"a sprite file added while the game is
running is not noticed until the next map load."* Live mode needs that memo invalidated for one path
on completion. `MapReloader` already establishes the precedent for hot-reloading content.

**Runtime mode should only ever run a local model.** A queue that depends on a remote API is a queue
that stalls when the network does, mid-session.

---

## 11. Improvements worth folding in

### 11.1 Author the atlas slices once, instead of hand-writing cell maps forever

Art packs ship a `.json` beside every atlas — `Environment/Tilesets/Wall_Tiles.json`,
`Icons/Resources.json`, and so on. **Checked, and it is not what it looks like:** each is a
single-frame Aseprite export describing the *whole* image (one frame rect covering 400x400, then
`meta`). No slice names, no per-tile rects, so nothing can be read from it yet.

What the pack does ship is the `.aseprite` **source** files. That makes a one-time authoring pass
possible: define slices in Aseprite, re-export, and the cell names and rects become data.

Why it is worth the afternoon:

- `extract_sheet.py` currently carries a hand-maintained `NAMES` dict mapping `(row, col)` to a name.
  Exactly the kind of thing that silently rots. The dict's own comment already encodes a design
  decision — that cell (0,0) is *"the church building, which is a multi-tile object and cannot be
  expressed in the current engine — skipped"* — which belongs in data, not a comment.
- Aseprite slices describe rectangles, not grid cells, so a multi-tile building is expressible as one
  named slice. A step toward `todo`'s "Allow multi-tile objects" rather than orthogonal to it.
- The cut stage stops being pack-specific and becomes "read the named rects" — the precondition for
  running the pipeline against an art pack it has never seen, which is the whole point of §3.

### 11.2 Keep the raw renders, always

`gen_tile.py` already defaults `--raw-dir` to `/tmp/air_tile_raw` and `tools/README.md` says to keep
them: *"When a result looks wrong, the first question is always 'is it the model or the downscale?',
and they answer it."* The pipeline must not regress this into a `tempfile` that is cleaned up.

### 11.3 A coverage report is a product feature, not just tooling

Once §3 is accepted, "this art pack covers 62% of the village data pack" is a number worth showing. It
tells an author what to generate next, and it tells a player whether a combination will look
half-finished before they start. `gap.json` is that number; it should probably be surfaced rather than
left in a file.

---

## 12. Staging

Each stage is useful alone, and none requires the next.

| stage | what it delivers | depends on |
|---|---|---|
| **0. Gap log** | a worklist of coverage misses, and the count from §11.3 | nothing |
| **1. Art root** | `art.pack` + `--art-pack`, replacing the one `constexpr`; data pack unchanged | nothing |
| **2. Palette lock** | generated art that matches the art pack's colour harmony | stage 1, plus a generated batch to snap |
| **3. Drained pipeline** | `gap.json` -> sheet -> cut -> palette, one command, writing art only | stages 0–2 |
| **4. Field split** | `sprite`/`art`/colours moved out of data definitions into the art pack | stage 1, and a format decision |
| **5. LoRA training** | offline, deterministic, API-free style reproduction | a coherent art pack with source-res art |
| **6. Live queue** | art arrives during play | `ResourceManager` memo invalidation |

Two notes on ordering:

- **Stage 0 first**, because it is cheap, invisible when idle, and the only stage that says what the
  feature is actually for. One church is one data point; whether agents ask for ten more buildings or
  ten thousand props changes the design, and today that is unknown.
- **Stage 4 is deliberately after the pipeline, not before it.** Moving the art fields out of the data
  definitions is the "proper" version of the split, but it is a **format migration** across 20 tile, 38
  object and 1 race file, and it buys nothing that stage 1 does not already buy — because stage 1 makes
  the *sprite root* independent, which is the part that changes what recombines. Doing stage 4 first
  would be a large migration funded before the gap log has proven the pipeline is worth having.
- **Stage 5 is deliberately late.** Most expensive, least urgent: the sheet-API backend reaches the
  same bar without it, and it is only worth the GPU hours once an art pack is coherent, has
  source-resolution art, and the volume justifies removing the API.

---

## 13. Open questions

Deliberately not resolved here.

1. **Does moving the art fields (stage 4) use a per-name override file or per-definition split?**
   A single `art/<artpack>/overrides.json` keyed by name is easy to layer and diff; splitting every
   definition into `<name>.data.json` + `<name>.art.json` is more honest about the model but triples
   the file count and needs a merge story. Not decided, and stage 1 defers it safely.
2. **Does the art pack own the font and chat colours?** `render.fontPath` is already a setting and
   `Defaults.h` holds chat colours. A dark fantasy pack plausibly wants both; they are art by
   §2's test, but they are also *legibility*, which is closer to accessibility than to style.
3. **Who reviews a generated batch?** §7.4 says nothing enters an art pack unreviewed. Human at a
   contact sheet, or a second model asked "does this match the art pack?" — open, and the contact
   sheet already exists so the human path is nearly free.
4. **Can an art pack be trained on its own generated output?** Tempting — it would let a pack
   bootstrap from one good batch. It is also the standard way to collapse a model into its own
   artefacts. Probably no, but it should be written down rather than assumed.
5. **Should the designer agent see the gap log?** Today `%MATERIALS%` is the data pack's catalog.
   Showing the designer the art pack's coverage would let it prefer materials that already have art —
   but §3.2 forbids letting that become a gate. Advisory-only is the constraint; whether it is worth
   doing at all is open.
6. **Does an art pack carry a semantic hint at all?** A "dark fantasy" pack might reasonably want the
   designer to favour certain materials. That is taste, not coverage, and it must not reduce the
   vocabulary. If it happens, it belongs in the art pack's `pack.json` as prose added to the designer
   prompt, clearly marked advisory.
7. **Multi-art-pack layering** (a base pack plus a small override pack) falls out of name-keyed
   overrides almost for free. Worth one line in `pack.json` when someone wants it; not now.

---

## 14. What not to do

- **Do not let an art pack change `character`.** It is the LLM's symbol table, not a sprite (§2.1).
  Changing it changes agent reasoning; colliding it corrupts `[SURROUNDINGS]`.
- **Do not let an art pack gate the data pack's vocabulary.** Swapping art must never change what can
  be built (§3.2).
- **Do not let the art pipeline write data definitions.** §4, and note that `gen_tile.py
  --emit-tile-json` does exactly this today — it is the first thing to fix.
- **Do not generate one asset per request.** Measured at 1,290 image tokens per output regardless of
  subject count, and internal consistency is lost. Batch.
- **Do not train on `data/sprites/`.** Derived, mixed-origin, 32x32. Train on the art pack's source.
- **Do not caption content on a style adapter.** One trigger token, §8.2.
- **Do not quantise after downscaling.** Quantise at 512, then `NEAREST` (§5).
- **Do not judge at 512px, or in an image viewer.** Contact sheet, then a 10x10 patch, then in-game.
- **Do not judge an art pack in isolation.** Judge it over a real data pack, because a half-covered
  field is the normal recombined state (§8.4).
- **Do not put diffusion in the frame loop.** §10.1.
- **Do not let generation write components it inferred.** `Interactable` vs `Obstacle` vs `Storage` is
  a behavioural decision. §7.4.
- **Do not let this touch `GameECS.cpp`'s system list, `AgentBrain`, or the command DSL.** It is a
  content pipeline. It reaches the game through the sprite root and the catalogs, and nothing else.
