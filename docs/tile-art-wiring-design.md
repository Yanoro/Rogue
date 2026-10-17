# Wiring generated art into the tile designer

**Status: the chain is built end to end, live and headless.** `tools/gen_tile_art.py`
generates and cuts; `AIRogue --tile-art` places and asserts both §7 contracts;
`Game::RefreshTileVisuals` hot-swaps into a running world; and `TileArtQueue` drives all
of it from inside the game on a worker thread, off by default and capped. The three
hot-swap checks are answered in §3 and §8's order is complete except for the backends
step 4 leaves for the LoRA.

The one thing still missing is the *backend choice* §5 flags: the live path runs the
Gemini sheet API because the local LoRA is blocked on the host CUDA fault, and §10.2's
rule ("runtime mode should only ever run a local model") therefore does not hold yet.
That is a deliberate, recorded trade for the demo — see §5 — not an oversight.

It assumes phase 1 is done: the tile designer agent produces tile definitions at runtime
and they render as their ASCII glyph. Phase 1 is described in
`.agents/tile-designer-handover.md`.

**Vocabulary, corrected 2026-10-17.** This pipeline is for **ground tiles** only. Walls, doors,
forges, anvils, troughs and other props are **objects**, never tiles; they use the object art path,
not this one. The shipped wall/anvil/forge/trough `data/tiles/*.json` entries and their `tile_*`
sprites are known debt (see [tile-and-object-vocabulary.md](tile-and-object-vocabulary.md)), not a
reason to route more props through the tile pipeline.

Read `docs/asset-generation-design.md` §7 (the pipeline) and §10.2 (the live queue)
alongside this; it is the general version, and this is the specific one.

---

## The prompt to start this off

> Read `docs/tile-art-wiring-design.md` first — it is the phase-2 design for wiring
> generated art into the tile designer, including the three hot-swap checks and two
> tensions left unresolved. Also skim `docs/asset-generation-design.md` §7 and §10.2,
> which this builds on.
>
> Phase 1 is done: a tile designer agent creates tiles at runtime and they render as
> their ASCII glyph. That glyph is a legitimate rendering, not a placeholder — the
> feature must work with art absent.
>
> Start with §8 step 1, the three hot-swap checks, **before writing any generation
> code**. The third is an unknown that decides the design: `Map::CreateTile` does not
> obviously create a render entity, and if it does not, there is nothing for a
> hot-swap to attach a `Sprite` to.
>
> Constraints:
> - Art goes to the generated layer, never the art pack. Agent-created definitions go
>   to the generated overlay, never the hand-authored catalog.
> - Palette-lock **before** the NEAREST downscale, never after (`tools/palette.py`).
> - Tile sprites are named `tile_<slug>.png` via `TileSpriteId`, not `<name>.png`.
> - Never block a frame. Mirror the AI path's background-thread → main-thread-queue
>   shape (`g_AIResponseMutex` / `g_AIResponseQueue`), and note
>   `.agents/known-issues.md` #13 flags those as process-global.
> - Add a headless `--tile-art <name>` mode so this is verifiable without a window.
> - Use the `agent_tasks.txt` lockfile; another agent works in this repo concurrently.
> - Do not attempt runtime verification of the AI path; the owner does that.

---

## 1. The chain

```
agent's tile design accepted (phase 1)
   -> a generation request for that tile
   -> generate        a sheet (Gemini now, local LoRA later)   tools/gen_tile_art.py
   -> palette lock    BEFORE the downscale                     }
   -> cut             to 32x32                                 } same script
   -> place           into the generated layer                 AIRogue --tile-art
   -> hot-swap        into the running world                   Game::RefreshTileVisuals
```

Two seams, and they are the two files:

- `tools/gen_tile_art.py` produces a `tile_<slug>.png`. It is a *script*, not a library
  call, because ImageMagick and Pillow are not on the default PATH here, a request is a
  network round trip of seconds to tens of seconds, and none of that belongs in a process
  with a 16ms frame budget. The seam is the file.
- `TileArt::SwapIn` takes a finished file plus a swap callback. That callback is where §4's
  main-thread handoff goes; today the only caller is the CLI's self-test.

**Cost, measured twice, at 1,290 image tokens and $0.0391 per request** — the same for a
one-cell sheet as for a four-cell one. That is the entire argument for batching, and the
reason the script sends exactly one request no matter how many names it is given.
`--dry-run` builds the prompt and stops, costing nothing; `--sheet` re-cuts a sheet already
paid for, so changing the inset or the palette is free. The raw sheet is always kept
(§11.2) — it is the only part of this that cost money.

## 2. Where the files go

- **Sprite** → the generated layer, never the art pack (§1.2, §4.2). With no art pack
  configured there is no generated layer, so `SpritePaths`' roots and
  `GapLog::DefaultPath()`'s derivation are the things to read — they already encode
  where "outside the pack" is.
- **Definition** → the generated overlay, never the hand-authored catalog. This is the
  owner's decision from the creation-path work; phase 1 should already have it.
- **Naming** — tile sprites are `tile_<slug>.png` (`TileSpriteId`), because tile names
  are ordinary words and a bare `water.png` would be ambiguous with the object system.
  The slicers write `<key>.png` unprefixed, so this rename is currently a manual step.

## 3. The hot-swap, which is more than a cache invalidation

Three things must happen. Only the first is obvious.

**Status: all three verified in the code, and the first two are now implemented.**
The check 3 investigation below is the one that decides the design, and its answer
was the opposite of what this section feared.

**1. Invalidate the missing-path memo.** `ResourceManager` caches failures so a world
renderer looking up a texture per entity per frame does not re-stat the filesystem.
`include/ResourceManager.h` states the consequence plainly: *"a sprite file added while
the game is running is not noticed until the next map load."* A `ForgetPath` is needed.
`MapReloader` is the precedent for hot-reloading content.

*Verified and implemented.* The memo is two containers, not one: `missing` records an
absent path and short-circuits in `TryGetTexture`, `textures` records a failed load
(a null `unique_ptr`) and is returned by `GetTexture`. **Both** have to be forgotten
or the other still answers "no texture" — a `ForgetPath` that cleared only the absent
mark would appear to work right up until the first sprite whose file existed but could
not be loaded. `ResourceManager::ForgetPath` (and `ForgetAll`, for a wholesale map
reload) clear both; the delete of the `unique_ptr` also unloads the stale texture,
which is what makes a *changed* file take effect and not just a new one.

Note where this happens: `Game::RefreshTileVisuals` forgets the resolved path before
re-attaching. Forgetting before the file exists, rather than after it appears, would
be wrong in the other direction — it would let the next lookup re-stat and re-memo the
absence, which is exactly the state being escaped.

**2. Attach `Sprite` to the entities that already exist.** This is the part that is
easy to miss. `AttachSprite` runs **at spawn**, and `Sprite` exists **only when the
image actually loaded** (`Components.h:119-135`) — which is exactly what makes the
ASCII art the fallback rather than a special case. So the entities that wanted this
tile have no `Sprite` component, and invalidating the cache alone would not give them
one. Something must walk the entities using that tile and attach.

*Verified and implemented* as `Game::RefreshTileVisuals(tileName)`, which returns the
number of cells that gained a sprite. It keys on the **tile**, and finds the cells by
asking the map (`Map::GetTile`) rather than by matching `DrawAscii` values. That
distinction is load-bearing: several **ground** tiles share the glyph `.` (floor, path, packed
earth, stone floor, wooden floor), so a glyph match would repaint cells belonging to a *different*
tile. The `DrawAscii` is rewritten from the tile anyway, so a cell whose tile changed keeps the
right glyph, and `Sprite` is removed before the attach so a failed re-resolve cannot
leave the previous texture on screen. This is the same refresh `PaintTile` already
performs, and deliberately not a second mechanism.

**3. Check whether a runtime-created tile has a render entity at all.** `Map::CreateTile`
pushes to `uniqueTiles`, sets the `Tile`'s fields, and returns it; `addTileToMap`
stores it in `tileMap`. But the **render** entities are created in `Map::Map`'s load
loop, not by `CreateTile`. If a created tile has no entity, a hot-swap has nothing to
attach to and must spawn one — which changes the design. **Verify this before writing
generation code.**

### The answer to check 3: there is no per-tile render entity, and there should not be

Read literally, the fear is true: `Map::CreateTile` (`src/Map.cpp:728`) does `tileRegistry->Get`,
builds a `Tile`, pushes it to `uniqueTiles` and `allTileInfo`, and returns it. It creates
**no entity**. `addTileToMap` (`src/Map.cpp:636`) is a single assignment into `tileMap`.
So a created tile has no render entity of its own. That is the whole of the finding, and
it does not matter — because **render entities are keyed to positions, not to tiles**, and
the map already has one per cell.

The load loop (`src/Map.cpp:344-359`) creates one child-of-`CurrentMap` entity per cell,
carrying `DrawAscii`, `ScreenPosition`, the `MapTileVisual` tag, and a `Sprite` when one
resolves. `Game::FindTileEntity` (`src/Game.cpp:877`) finds it by `ScreenPosition` and
identifies it by being a child of the map entity with no `GamePosition` — the discriminator
that separates ground from what stands on it. `TickIndicators`-style per-cell entities are
not involved.

So the three possible designs collapse to one:

| design | verdict |
|---|---|
| spawn a render entity per created tile | **unnecessary** — the visual is per cell, and every cell that can draw the tile already has one |
| cache a pointer from a `Tile` to "its" entity | **wrong** — one tile is drawn by many entities, and which cells use it changes as the map is painted |
| **sweep the cells drawing the tile and refresh their existing entities** | **this one** — and `PaintTile` already does exactly it for one cell, so the hot-swap is that code applied to every cell of a tile |

The consequence for the rest of the design is a simplification: **a hot-swap needs no
entity creation at all**, so there is no new lifetime to own, no orphan when a map
reloads, and nothing to serialize. `RefreshTileVisuals` is ~40 lines because the only
new thing in it is the loop.

Two smaller findings from the same read, both useful later:

- **`PaintTile` resolves sprites at paint time** (`src/Game.cpp:933`), which means a tile
  painted *after* its art exists gets the sprite with no hot-swap at all. `PaintTile` is
  therefore the second consumer of the same refresh, and the reason the refresh lives on
  `Game` rather than inside the tile designer's window.
- **`MapReloader` is not a precedent for this.** It reloads the map file (`LoadMap`),
  which re-runs the load loop and rebuilds every visual from scratch. It never invalidates
  `ResourceManager`, so it "fixes" a missing sprite by accident, through rebuilding — and
  it leaves the memo holding a stale answer for the new world. `ForgetAll` now exists for
  that, though nothing calls it yet: making map reload clear the memo is a behaviour change
  (sprites that were remembered as bad would be retried) and is left as its own decision.

## 4. The queue

Generation is 10–30 s locally, or a few seconds plus a network round trip for the API.
Never block a frame.

The engine already has the shape to copy: the AI path pushes work to a background
thread, which takes a mutex and pushes a lambda onto a main-thread queue
(`g_AIResponseMutex` / `g_AIResponseQueue` in `src/GameECS.cpp`). Mirror it — and note
`.agents/known-issues.md` #13 already flags those globals as process-global rather than
per-world, which matters if a second world is ever created.

The glyph is not a placeholder to tolerate; it is a legitimate rendering of the tile,
and it is present from the moment the design is accepted. The game never waits.

## 5. Two tensions the earlier doc did not resolve

**Remote vs local.** `docs/asset-generation-design.md` §10.2 says: *"Runtime mode should
only ever run a local model. A queue that depends on a remote API is a queue that stalls
when the network does, in the middle of a session the player is watching."* But the
chosen backend for art is the Gemini sheet API, because it works today and the local
path is blocked on a host CUDA fault. For **demoing the wiring**, remote is fine. For
the **shipped feature**, the doc's rule stands. Decide this consciously rather than
inheriting the demo's choice.

**Review.** §7.4 says nothing enters the generated layer unreviewed. A runtime hot-swap
is auto-accept by definition, so the two cannot both hold. §13.3 flagged this as open.
A defensible resolution for tiles specifically: auto-accept, because a `TileDef` carries
no economy component (`Components.h:181-201`) so the blast radius is a decorative
surface that blocks or does not — and make the generated layer inspectable and
deletable so a bad result is recoverable.

## 6. Cost, and why batching still matters

Measured: **1,290 image tokens per output regardless of subject count**, ~$0.04. So one
request per tile is roughly 19× the per-asset cost of a batched sheet. If agent
creations are rare, per-tile is acceptable. If they are common, accumulate requests and
drain in batches — the gap log is already a drainable worklist, so the shape exists.

## 7. Verification

Add a headless mode next to `--gaps` and `--tile-design`, e.g. `--tile-art <name>`,
running the whole chain and reporting the path written. This repo has no test framework
and cannot run the game headless, so anything reachable only through ImGui is
unverifiable — that is why `--gaps` exists.

Assert two contracts while you are there:
- **every opaque pixel of the output is in the palette** (`tools/palette.py report`
  prints the recolour share; the locked set should be 0% outside);
- **the sprite landed in the generated layer, not in the pack** — the rule from §1.2,
  enforced rather than trusted.

### Built, and what it had to get right

`AIRogue --tile-art <name>` reports the convention id, both art roots, where the
sprite would be written, what currently resolves, and whether the art pack already
wins. Adding `--tile-art-sprite <png>` places the file and runs the two contracts;
`--tile-art-palette <json>` overrides the derived palette.

Three things the implementation had to settle that this section left open:

- **The palette check cannot use the reported percentage.** `palette.py report` rounds
  it to one decimal, so one stray pixel in a 1024-pixel sprite prints as `0.0%` while
  the count beside it says `1 opaque pixels are not in the palette`. A contract of
  "every opaque pixel is in the palette" decided on the rounded figure **passes a
  sprite that violates it** — measured, on a deliberately damaged fixture. The verdict
  uses the exact count; the percentage is kept for the human-readable line.
- **It must measure one sprite, not the directory.** `report` takes a directory, and
  the generated layer accumulates every generated tile, so measuring there would make
  this sprite's verdict depend on what else had been generated. The check stages a
  temporary directory holding exactly the file and removes it afterwards.
- **A vacuous pass is a failure.** A sprite that measures as having no opaque pixels
  cannot violate the contract, so it must not pass either; that is the shape a broken
  or unreadable file takes.

The second contract is enforced in `TileArt::PlaceSprite`, not merely reported: it
refuses when the source *is* the pack's file and when the destination resolves into the
pack, and `--tile-art` re-checks the written path against the pack root afterwards so a
future change to the root derivation cannot quietly move the write inside it.

### The hot-swap needs a world, so it gets its own mode

The file half is provable with no display; the swap half is not, because it needs the
ECS and a window. `--tile-art-selftest` therefore runs the game, and the sequence is
the one that cannot be checked statically:

```
settle the world            (every tile's sprite is resolved; the absence is memoised)
place the sprite            (the file appears while the world is running)
Game::RefreshTileVisuals    (the swap, on the main thread, between frames)
```

Measured on a 10x10 map with nine cells of a runtime-created tile whose art did not
exist:

```
before:     resolves=(nothing)  cells-with-a-sprite=0
placed:     .../generated/testpack/sprites/tile_marble_floor.png
after:      resolves=.../generated/testpack/sprites/tile_marble_floor.png  cells-refreshed=9
hot-swap:   ok (9 cell(s) gained a sprite after the file appeared)
```

Nine from nine is the evidence: had `ForgetPath` been missing the count would be zero,
and had the attach loop been missing it would also be zero. Both failure modes are
indistinguishable from a pass without the world, which is why this mode exists at all.

### Seeing the pipeline

The three panels under the Tile Designer's art section read the artifacts the
generator already leaves on disk. None of them change the pipeline; they exist
because every stage's output was previously invisible, and "why is this tile
wrong" is a question the files answer and a log line does not.

| panel | what it shows | why |
|---|---|---|
| **Pipeline artifacts** | every file produced, with its path and a copy button, plus **the prompt that was actually sent** | the prompt is what a disappointing sheet has to be judged against, and nothing else surfaces it. It was written to disk and read by nothing |
| **Pixels and palette** | the placed sprite drawn one rectangle per pixel at any zoom, a toggle to redraw it as its nearest palette colour, and the palette with the entries this sprite uses boxed | the palette contract was a number in stdout ("0.0% outside"). Drawn, "28 of 128 colours, here they are" is checkable by eye, and a pixel outside the palette is visible instead of counted |
| **Pipeline stages** | sheet → per-cell crop → trim → palette lock → final 32×32, as thumbnails, each labelled with its source size | this is the processing, and it is the only view of the model's raw output at any size. The stage sizes are the only place the sheet's resolution appears at all |

Five stages are labelled rather than two because the pipeline has seven, and the two
that are not images are named in the artifacts panel: the prompt (stage 0) and the
inset, which is arithmetic on the crop rather than a file of its own.

Three things worth knowing about the implementation:

- `Job::worklistPath` is what makes this possible. Everything the pipeline produced
  hangs off the worklist by convention -- `tile_art_sheet.png`, its `.prompt.txt`,
  `cell_<n>_{crop,trim,locked}.png` -- so the queue records the worklist path on every
  job it dispatches, and the window resolves the rest from it. Nothing new is written
  for the UI.
- Thumbnails are resized on load (nearest for a ≥4× reduction, bilinear below that):
  nearest-neighbour *downscaling* of a busy texture aliases into noise rather than
  sampling it, while nearest is exactly right for pixel art going up.
- `raylib::Image`'s constructor **throws** on a path it cannot load rather than
  returning a null image. A job that has not placed a sprite yet has an empty path,
  which is the common case, so both image loads are guarded -- an unguarded one takes
  the window down rather than reporting a missing sprite.

## 8. Order

1. ~~The three hot-swap checks in §3, especially the third.~~ **Done** — see §3. Check 3's
   answer is that there is no per-tile render entity and none is needed: visuals are keyed
   to positions, so the swap needs no entity creation.
2. ~~The headless chain (§7), so the pipeline is provable without a window.~~ **Done.**
   `tools/gen_tile_art.py` generates and cuts; `--tile-art` places, asserts both contracts,
   and reports; `--tile-art-selftest` drives the swap against a loaded world. Verified on a
   real request: four tiles, one request, $0.0391, all four palette-locked and in the
   generated layer, and nine map cells gained sprites after the file appeared.

   Two things this step learned that the design had not anticipated:

   - **The prompt needs the ground/prop distinction, and the old taxonomy was wrong.** "Seamless,
     must tile" applied to an `anvil` or a `forge` produces a *grid of anvils* — the first generated
     sheet came back exactly that way, because those props had been misclassified as tiles and a
     `TileDef` has no field that says "this is a thing, not a surface" (`Components.h:181-201`).
     **The fix is the vocabulary, not another tile field:** an anvil or a forge is an **object** and
     goes through the object art path. A genuine ground tile with a prop baked into it still needs
     the word list/description heuristic, but that is a rendering-quality fallback, not a licence to
     add walls, doors or furniture to `data/tiles/`.
   - **A generated ground tile can be weaker than the surface tiles around it, and that is
     survivable.** The puddle came back as a puddle inside a sandy ground texture; the palette
     contract still passes and the tile still reads. This is the argument for the owner reviewing
     the cut sprites before committing them, not for more pipeline.

3. ~~The queue and the window wiring (§4).~~ **Done.** `TileArtQueue` is the queue; it
   is a member of `Game`, not a file-scope global, so `.agents/known-issues.md` #13's
   process-global problem is not inherited (the AI path was left alone — it works, and
   changing it is not this step's job). The pump is an ECS system registered by
   `ECSInitTileArtSystems`, the completion callback is `Game::HandleTileArtGenerated`,
   and the Tile Designer window has a "Generate art" button with the queue's state
   under it.

   **Automatic request, so a prompt is the whole instruction.** The button is off the
   critical path: `autoRequestArt` makes accepting a tile also queue its art, in the
   same step, so `a mossy stone floor that blocks movement` produces the definition
   *and* the sprite with no second click. Three things make that safe rather than
   surprising:

   - it fires in the accept branch, on the main thread, at the only moment the name is
     guaranteed to be in the catalog for the swap to attach to;
   - it goes through `Game::RequestTileArt`, so **batching still applies** — designing
     four tiles in a row is still one request, which is what makes leaving it on
     reasonable;
   - both outcomes are logged (`ART REQUESTED (automatic)` / `ART NOT REQUESTED: <why>`),
     so a refusal appears in the transcript instead of looking like an ignored prompt.

   It defaults to following `art.generation.enabled` on a checkout that has never
   stored the preference (`DebugWindowState::HasTileDesignerAutoArt` distinguishes
   "turned off" from "never said"), so enabling generation is one decision rather than
   two; the checkbox in the window overrides it and persists.

   The decisions in it:

   - **Batching is enforced, not offered.** One request costs the same whether it
     carries one tile or nineteen, so `batchDelaySeconds` collects everything asked for
     in a window into one request — measured at 3 tiles → 1 request. One request per
     tile is ~19x the cost per asset, which is the kind of thing nobody notices until
     the bill arrives.
   - **The cap is in requests, not tiles**, because requests are what cost:
     `maxRequestsPerSession` (default 8, ~32 cents). A job past the cap is marked
     `Skipped` with the reason, never silently dropped.
   - **The command is external** (`art.generation.command`, `{worklist}` substituted).
     ImageMagick and Pillow are not on the default PATH, and the local-LoRA path has to
     stay swappable behind the same seam, so the game writes a worklist and runs a
     command that answers with `<worklist>.result.json`. A command that dies produces a
     *result file* saying so, rather than a silent nothing.
   - **The worker posts data, not a lambda.** `TileArtQueue::Completion` is a struct of
     names and an error string; the AI path's lambdas capture `flecs::entity` and need
     `entity.is_alive()` guards because of it. Nothing here captures world state.
   - **`Stop()` is bounded** (30s) and joins before the ECS goes down, because a batch in
     flight posts a completion whose callback touches the world.

   Verified without spending anything, using `tests/stub_tile_art.py` (pure stdlib,
   writes a flat palette-locked PNG) as the command: request → worker → drain → swap end
   to end in-game, `before: 0 cells` → `after: 9 cells`, frames running throughout. In
   isolation the queue passes 21 checks: 3 tiles batched into 1 request, a duplicate
   refused while queued, a request refused past the cap with the limit named, everything
   refused while disabled, and a job whose command exited non-zero failed with the
   missing file named.
4. Swap the backend to the trained LoRA behind the same seam — once the host CUDA fault
   is cleared. See `~/nixos/comfyui-docker.sh`'s comments for what was already tried.
