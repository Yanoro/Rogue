# The sprite designer: tiles and objects

**Status: all seven phases are built and green.** The designer creates tiles and objects
singly or as sets, validates them per entry, names their art per kind, and hands a set to the
queue in requests of at most `art.generation.maxCellsPerRequest` sprites. The designer
window now designs both kinds — a mode dropdown the router can set and the user can
correct, a per-kind preview, and a per-kind rewrite brief — and every piece is reachable
headlessly: `--sprite-prompt`, `--sprite-design`, `--sprite-design-overlay`,
`--sprite-route`, `--object-check`, `--sprite-designer-objecttest` and the existing
`--tile-designer-logtest`.

**Four design guesses have now been killed by measurement, which is why they were written
as guesses.** Phase 3's free sheet measurement cannot be made (§16); phase 4's numeric
`count` question was answered wrongly on real requests, so it became a size class (§17);
phase 5 found the object preview cannot be an editor on the same terms as the tile one
(§18); and phase 7 confirmed in the running code that an object's art was being generated
under the *tile* naming convention, so every sprite the designer paid for an object was
written to a filename nothing loaded (§19).

**Phase 7 was taken out of order at the owner's request, before phase 6.** The reason is
recorded rather than the ordering: the object-art naming bug made "Art automatically"
spend real money for a sprite the engine could never resolve, and testing the window while
that was true would have meant testing a bug. Sheets can wait; a silent payment cannot.

The build records are at the end: §14 (phase 1: the kind table and the prompt split), §15
(phase 2: object validation and runtime registration, including a wrong turn worth not
repeating), §16 (phase 3: the measurement that cannot be done, and what was done instead).
Everything before them is still a specification to build against. Where a number is
measured it is marked as such; where a number is a guess it says so.

The reader this is written for is whoever implements it — including a later session that
has none of this conversation. So it records not only what to build but *why the obvious
alternative was rejected*, because the alternatives here are all reasonable and several
of them were nearly chosen.

Read `docs/tile-art-wiring-design.md` and `docs/asset-generation-design.md` first. This
document is the third in that series and deliberately does not repeat their contents: the
generation chain, the palette rule, the hot-swap and the queue are all as they are there.
What is new here is **a second kind of thing to design, and a classifier that decides
which kind a request is**.

**Vocabulary, corrected 2026-10-17.** A **tile is ground only**. Walls, doors, roofs, beds,
anvils, forges and everything else that stands on or over the ground are **objects**, never tiles.
The classifier must answer `object` for a wall/door request and the object grammar must accept it.
Some build records below predate the rule and record a wrong answer or a wrong expectation; they are
annotated in place. The canonical statement and the migration record are in
[tile-and-object-vocabulary.md](tile-and-object-vocabulary.md).

---

## The prompt to start this off

> I'd like to now extend the tile designer's skills to also include objects. Maybe rename
> him to sprite designer. I think one way we could do it is giving the initial prompt to
> jev so he can categorize in either a tile or an object prompt. This way we can have
> different starting prompts for each case. Then i imagine the workflow would be similar
> to the current one tiles has. Another feature i want to implement in the future is
> allowing the creation of an entire sprite sheet at the same time, for instance by asking
> for a prison related sprite sheet we can get a multitude of sprites at the same time
> instead of asking for individual tiles.

And the follow-up that fixed the router's shape:

> What do you think of also asking the s1 model if the request was a single or multiple
> sprites? That way we could refine the prompts for each case

---

## Decisions, and who made them

The owner answered six questions before this was written. They are settled; do not
re-litigate them without asking.

| # | Decision | Chosen |
|---|---|---|
| 1 | Jev's role | **One-shot router** at the start of a new design. Classify once, pick the prompt, then the conversation behaves as it does today. Not per-message, not a clarifier. |
| 2 | Overriding it | **Jev first, with a visible dropdown to correct it.** A wrong classification costs one decision call, not a wasted generation. |
| 3 | Object power | **Recombine the ten registered object components and their parameters.** Nothing may be invented that the engine cannot simulate. |
| 4 | Sheets | **Both, with sheets first-class.** "a prison sheet" yields several named sprites; "a ratty prison blanket" stays single. |
| 5 | Created objects | **Same as tiles**: they join the object catalog and are usable by blueprints and spawns. Nothing is placed automatically. |
| 6 | Renaming | **User-facing only.** The window, prompt prose and docs say "sprite designer"; `TileDesigner*`, `tileDesignerPromptFile` and `TILEDESIGNER.md` keep their names, and `--tile-design` keeps working. |

The follow-up is also settled: the router asks **single-or-multiple and how many** in the
same call as the kind, so the prompt can be chosen for the case rather than the case being
inferred from what the model happened to send (§3).

---

## 1. What a "kind" is, and why it is a table

Two things are being designed now — a tile and an object — and at least two more are
plausible later (an item, a piece of decor). The failure mode to avoid is a boolean
`isObject` threaded through the window, the validator, the prompt builder and the art
queue, which is how a system ends up with four places that disagree about what an object
is.

So a kind is a **descriptor**, and every piece of per-kind behaviour is one field in it:

```cpp
// include/SpriteKinds.hpp -- proposed
namespace SpriteKinds {

enum class Kind { Tile, Object };

struct Descriptor {
  Kind kind;
  const char *id;               // "tile" | "object" -- the classifier's answer key
  const char *label;            // "Tile" | "Object" -- what the window shows
  const char *promptFile;       // which instruction document to build from
  const char *overlayDirectory; // where an accepted definition is written
  const char *noun;             // "tile" | "object", for prose and refusals
};

const Descriptor *All();                        // the table, for the dropdown
const Descriptor *For(Kind kind);
const Descriptor *Parse(const std::string &id); // nullptr when the id is unknown

}  // namespace SpriteKinds
```

Adding "item" later is then a row and a prompt file. That is the whole argument for the
table: it is not abstraction for its own sake, it is the difference between one new row
and a fourth `if`.

**The descriptor holds no validator.** Validation differs per kind in a way a function
pointer would hide badly — a tile is refused for a two-character glyph, an object for an
unknown component — and both need to report *which* field failed and why in a sentence
fed straight back to the model. So each kind keeps its own validator in its own file
(`TileDesigner`, `ObjectDesigner`) and the descriptor names it only as "the thing to
call" via a small dispatcher (§5).

---

## 2. The taxonomy is two axes, not one

The router answers two independent questions:

```
                single                        set
   tile    "cracked clay floor"          "a prison's floors and flagstones"
   object  "a wooden crate"              "a prison's furniture and fittings"
```

Conflating them would be the easy mistake. A request can be a single object, a set of
tiles, or anything between, so the prompt is selected by a **pair**, and the instruction
documents are organised so that the four combinations come from two files with two
sections each rather than four documents that drift apart:

| file | section | selected when |
|---|---|---|
| `data/tiles/TILEDESIGNER.md` | *one tile* | kind = tile, shape = single |
| `data/tiles/TILEDESIGNER.md` | *a set of tiles* | kind = tile, shape = set |
| `data/objects/OBJECTDESIGNER.md` | *one object* | kind = object, shape = single |
| `data/objects/OBJECTDESIGNER.md` | *a set of objects* | kind = object, shape = set |

The shared parts — the reply contract, the refusal protocol, the rule that the fenced
block is JSON and nothing else — stay in one place per file, because they are identical
across the single/set split and duplicating them is how the two halves of a document
start disagreeing.

---

## 3. The router

One `Ask` call, **three typed questions**. The protocol takes a map of questions and
returns a map of typed answers, so this is one decision and not three.

```cpp
// include/SpriteRouter.hpp -- proposed
namespace SpriteRouter {

// What the classifier was asked and what came back, resolved into engine terms.
// Every field records "unanswered" distinctly from "answered low" -- the protocol
// returns no confidence for some answer types, and a reader that cannot tell the two
// apart will invert the meaning of the field.
struct Verdict {
  // null when the `kind` question was unanswered or its key was unknown.
  const SpriteKinds::Descriptor *kind = nullptr;
  // False when `shape` was unanswered.
  bool shapeAnswered = false;
  bool sheet = false;
  // The model's count, rounded from the Value answer's weighted level index, clamped
  // to [1, kMaxSetSize]. 0 means unanswered.
  int requested = 0;
  // Confidence is *absent* on Choice answers (the protocol defines it only for the
  // other primitives), so 0 here means "did not say" far more often than "unsure".
  float kindConfidence = 0.0f;
  float countConfidence = 0.0f;
  std::string theme;  // e.g. "prison", for the set prompt. Empty when not given.
  std::string why;    // the answer's own words, shown as-is rather than parsed.
};

using Callback = std::function<void(Verdict)>;

// Never blocks; `onDone` always fires exactly once, including on transport failure,
// an HTTP error, an unparseable body and cancellation -- SystemOneClassifier
// guarantees it, and this resolves the answers before handing them over so the
// caller never sees a raw `Answer`.
void Classify(SystemOne::SystemOneClassifier &classifier, const std::string &request,
              const std::string &vocabulary, Callback onDone, std::stop_token stoken = {});

}  // namespace SpriteRouter
```

Three questions, in **one** `Ask`:

| id | `OptionType` | protocol primitive | shape of the question |
|---|---|---|---|
| `kind` | `Choice` | `choice` | "Is this request for a tile or an object?" keys `tile`, `object` |
| `shape` | `Choice` | `choice` | "Is this one sprite or a set?" keys `single`, `set` |
| `count` | `Value` | `score` | "How many sprites does it ask for?" levels `1`..`8` |

**`count` is a `Value`, not a `Score`.** This is easy to get wrong because the two names
collide: `OptionType::Score` maps to the protocol's `noul`, which is a bare yes/no, while
the protocol's `score` primitive is an ordered scale and is this project's
`OptionType::Value` (`include/SystemOneRecorder.h:62-64,72-82`). The distinction that
bites is confidence: **`Score` answers carry none** — the protocol defines the primitive
without one, and `Answer::confidence` is left at 0 for them
(`include/SystemOneRecorder.h:135-138`). Its answer is a probability-weighted mean of level
indices, so the count is that mean rounded using the documented levels, and **never** a
host-side divide by the level count — `Answer::score` carries an explicit warning about
that off-by-one (`include/SystemOneRecorder.h:117-121`).

`kMaxSetSize` is 8 because the protocol allows 2–10 `Value` levels and 8 is the useful
end of that range for a sheet (§4). It is deliberately not 10: the tenth level would be
an invitation to ask for a sheet that cannot be cut legibly.

`state` carries the request text plus the vocabulary the classifier needs to judge with —
for objects, the ten registered component names and their parameters (§5), because
"a crate that holds things" is only classifiable as an object if the classifier knows
`Storage` exists.

### Three things the implementation must not get wrong

- **Zero confidence means "unanswered", not "unlikely".** The protocol returns no
  confidence at all for some answer types, and `JevClassifier` deliberately writes a
  missing confidence as `0` rather than defaulting to `1`
  (`include/JevClassifier.h:44-45`). A window that reads `0` as "very unlikely" would
  silently invert the meaning of the field.
- **A `choice` answer is the option's key, not an index.** The keys are chosen by the
  caller (`tile`, `object`), which is why `SpriteKinds::Parse` exists and why the raw
  answer is never cast to a number. The protocol has no "none of these" answer, so an
  unsure model may still return a key — a caller that needs "neither" must ask for it as
  an explicit option (`include/SystemOneRecorder.h:123-126`).
- **An unanswered question must not be guessed.** A null `kind` is a state the window
  resolves by falling back to the mode already in the dropdown and saying so, not by
  picking the more likely kind.

### What it buys, in order of importance

1. **The right prompt is built from the start.** The model is told "define one object"
   before it writes, rather than being corrected after it writes a tile.
2. **The set prompt can carry count and theme.** "8 pieces of prison furniture and
   fittings" is a coherent brief; "some prison things" is eight unrelated guesses.
3. **A pre-flight estimate.** Once the verdict is known the window can say what it is
   about to spend *before* spending it (§8). This is the only feature in the project that
   costs money by default, so making the cost visible is not a nicety.

### Fallback if `count` proves noisy

`Value` answers are the ones most likely to come back unusable for a question this
open-ended — a weighted level index over a scale the model has to interpret. If logged
answers turn out to be nonsense, demote `count` to a `Choice` of size buckets (`small` ≈ 4,
`medium` ≈ 8) and let the set prompt pick a number inside the bucket. Note that a `Choice`
answer carries **no confidence at all** (`include/SystemOneRecorder.h:135-138`), so the
fallback trades a hedged number for a crisp bucket — which is the right trade for a prompt
that only needs a rough size.

The raw answers are worth recording from day one precisely so this is a data question
rather than an argument: `SystemOneRecorder` already records decisions, so observing them
costs nothing.

---

## 4. Measure the sheet free, before any prompt depends on it

This is sequenced first among the interesting work because it is **free** and because the
set prompt's wording depends on the answer.

The cost model is flat per request: **1,290 image tokens (~$0.039) regardless of how many
cells the sheet holds** (measured, `docs/asset-generation-design.md`). So more cells per
sheet is strictly cheaper per sprite, and the constraint is not cost but legibility:

| cells on one sheet | request cost | per-sprite cost | per-sprite detail |
|---|---|---|---|
| 1 | $0.039 | $0.039 | best — the whole cell is one subject |
| 4 | $0.039 | ~$0.010 | good — the four generated surface tiles are usable (measured) |
| 9 | $0.039 | ~$0.004 | **unknown** |
| 16 | $0.039 | ~$0.002 | **likely bad (guess)** |

Only the first two rows are observations. The rest is extrapolation and must be measured
before the set prompt starts asking for large sets.

**How, at no cost:** `tools/gen_tile_art.py --sheet <existing.png> --cols N` re-cuts a
sheet that has already been paid for. The 4-cell surface sheet from the tile-art work is
on disk (`data/generated/work/tile_art_sheet.png` and its siblings), so cutting it at 4
and at 16 cells and comparing sprites per cell answers the question with no request. The
result sets `SHEET_CELL_CAP` — a new constant this document proposes, defaulting to 4
until the measurement justifies more — in the art path and the wording of the set prompt.

> **CORRECTION (phase 3).** The paragraph above is wrong and is kept only so the mistake is
> not repeated. Re-cutting a sheet that holds **four** subjects on a **16**-cell grid does
> not produce a 16-cell sheet: it slices each of the four existing sprites into quarters.
> What degrades as N grows is the *model's* behaviour — whether it still honours "one
> subject per cell" rather than collapsing into the repeated-motif failure the `puddle`
> tiles already showed — and no re-cut of fixed pixels can measure that. Measuring it
> needs real generations at those sizes, which costs money. See §16 for what was done
> instead and where the measurement now happens.
>
> The table above is also contradicted by the measurements taken in phase 3: the sheet
> size is chosen by the model and does not scale with the cell count, so the 4-cell
> sprites (which came out usable) had 6.2 source px per output px while the 1-cell sprite
> that went wrong had 32.0. Resolution is not the predictor this section assumed, and
> `SHEET_CELL_CAP` is not a resolution question.

Two related findings that already exist and matter here:

- A cell that should hold one subject can come back as a **repeated motif** — the
  `puddle` / `self test puddle` tiles both did. More cells per sheet makes this more
  likely, not less.
- `--cols` is currently derived as `ceil(sqrt(N))` (`tools/gen_tile_art.py:868`), so the
  grid is always as square as possible. For a set, a deliberate shape is better: eight
  sprites as 4×2 rather than 3×3-with-one-empty, because an empty cell is wasted sheet
  area and because a ragged grid is harder to obey.

---

## 5. Object definitions, and where the engine's vocabulary is enforced

Objects are already data-driven. An `ObjectFactory` template is JSON whose `components`
array names registered components, and `RegisterDefaultComponents` registers **ten**
(`src/ObjectFactory.cpp:20-151`). A first draft of this document said eight; the count was
wrong because it stopped at the six obvious ones plus two, and an early phase-1 check
found `Requires` and `Trains` in the same function. The lesson is worth keeping: this
table is read from the code, not from memory.

| component | parameters (JSON keys) | what a designer may express with it |
|---|---|---|
| `Interactable` | — | it can be used |
| `Obstacle` | — | it blocks movement |
| `Portable` | — | it can be picked up and carried |
| `Storage` | `capacity` | it holds items |
| `Harvestable` | `amountRemaining`, `timer`, `durabilityCost`, `drops[]` | it can be gathered from |
| `Workstation` | `recipes[]` | it is used to craft |
| `Evolvable` | `evolveTime`, `evolveTarget`, `isActive` | it changes into another template over time |
| `Placeable` | `placement{becomes, surface[], maxDistance}` | it can be put down in the world |
| `Requires` | `requires` (equipped item tags) | using it needs equipment |
| `Trains` | `trains[]` (skill grants) | using it teaches a skill |

All ten are in use in `data/objects/` today, so none is dead vocabulary: `Portable` ×10,
`Interactable` ×25, `Obstacle` ×18, `Workstation` ×4, `Storage` ×2, `Evolvable` ×2,
`Harvestable` ×3, `Trains` ×2, `Requires` ×1, `Placeable` ×1.

Note the per-component key names — `evolveTarget`, not `target`; `amountRemaining`, not
`amount`. They are the JSON keys the setters read, so a prompt that paraphrases them
produces definitions the engine silently defaults. The vocabulary handed to the designer
should be generated from the setters where practical rather than transcribed into prose.

A created object therefore looks like:

```json
{
  "name": "wooden crate",
  "description": "A nailed crate with a loose lid that holds things.",
  "components": ["Obstacle", "Storage", "Portable"],
  "capacity": 12,
  "art": ["+--+", "|++|", "+--+"],
  "palette": { "+": [139, 69, 19, 255], "-": [110, 55, 15, 255] },
  "character": "c",
  "characterColor": [176, 132, 82, 255],
  "backgroundColor": [46, 34, 24, 255],
  "designerNotes": "..."
}
```

`designerNotes` is stripped before writing, exactly as it is for tiles.

### Validate by running the real setters

The temptation is to write a second parser that checks component names against a list and
parameter shapes against a schema. **Do not.** The loader already knows what it accepts,
and a second implementation is a second thing that can disagree with the one that runs.
The validator should build a throwaway entity in a scratch world and run the same
`componentSetters` the loader would, so "the validator accepts it" and "the engine can
spawn it" are the same statement by construction.

The refusals that then come for free, plus the ones that need saying explicitly:

- an unknown component name, named, with the closest known one suggested;
- a parameter the component's own setter rejects (a `capacity` of `-1`, a missing yield);
- no components at all — an object that does nothing is ground, i.e. a tile, and that belongs in
  the object prompt's "when this is really a tile" paragraph;
- an `art`/`palette` pair where the art uses a glyph the palette does not define, which
  draws as a blank;
- a name already taken **in this kind's namespace** (§6).

### The gap that must be filled: runtime registration

`TileRegistry` has a runtime-add path (`AddRuntime`, `IsRuntimeAdded`, `RuntimeNames`,
`RemoveRuntimeTiles`, a catalog revision counter) and its own pointer-stability care.
`ObjectFactory` has **nothing equivalent**: templates are loaded once at startup by
`LoadTemplates`, and the only mutation is `RemoveTemplate`. So the object half needs
`AddRuntime`, `IsRuntimeAdded`, `RuntimeNames` and `RemoveRuntimeTemplates` mirroring the
tile registry's contract, including returning a result that says *why* a definition was
refused rather than a bare `bool`.

One bug from the tile registry is worth copying the fix for rather than the mistake: the
catalog loader once filed every tile under an empty key because
`tiles[def.name] = std::make_unique<TileDef>(std::move(def))` evaluated the move before
the key (`.agents/tile-designer-handover.md`). The object map has the same shape and the
same trap; copy the key out first.

A second, related bug worth knowing about because objects are containers: value-owned
maps are fine, but anything handing out a pointer into the template map invalidates it on
insert. `ObjectFactory::GetTemplates()` returns a reference and its comment already
warns that a caller must not hold it across a mutation — the runtime-add path must not
turn that warning into a crash by inserting while a palette loop is iterating.

---

## 6. Names: two namespaces that may overlap

**`anvil` used to exist as both a tile and an object.** The 2026-10-17 migration retired
`data/tiles/anvil.json`; `data/objects/anvil.json` is now the only one. The two registries still
allow a cross-kind name collision in principle, so the warning below remains useful, but the shipped
example of it is gone.

Therefore:

- name uniqueness is enforced **per kind**;
- when the other kind already uses the name, the window **warns and allows it**, because
  the two live in different registries and the engine resolves them separately;
- the sprite conventions stay distinct: a ground tile is `tile_<slug>.png`, an object is
  `<slug>.png`, so a future cross-kind pair would not collide on disk.

A note for whoever implements the warning: "which anvil do you mean" is a real question
an agent reading the world will ask, so the warning is not cosmetic. It should say which
registry the collision is in. There is no shipped collision today.

---

## 7. Art: nearly free, with one naming fix

`ObjectFactory` already resolves an object's sprite through
`AttachSprite(obj, explicitSprite, type, *resourceManager, debugLog)`
(`src/ObjectFactory.cpp:282`), using the template key as the convention id. So an object
resolves `<slug>.png` from the art pack and then the generated layer, with **no new
resolution code and no new naming convention**: two conventions already exist, one per
kind, and the generated overlay already serves both.

The fix is on the generation side, and it is small but load-bearing:

- **`TileArt::PlanFor(tileName)` derives `TileSpriteId(tileName)` internally**, so it
  would write an object's art to `tile_wooden_crate.png` — a file nothing ever looks up.
  A job must therefore carry its **convention id**, not just a name.
- **`is_surface()` guesses from the name and description**
  (`tools/gen_tile_art.py:271`), matching a word list that includes `"wall"`, `"stone"`
  and `"roof"`. Those are exactly the words that should force an **object**: a stone wall or
  a roof is not ground. An object called *stone wall* or *bell tower* would be heuristically
  misread as a tile; with a sheet mixing kinds it is worse. The job must carry its **kind**,
  and the generator must use it instead of guessing.

**The invariant this must preserve** is the one the tile path already states: the sprite
is named the same way by the writer and the reader, and the pack beats the generated
layer. Both halves already exist; the change adds a discriminator to a path that is
currently hardcoded:

```cpp
// today -- include/TileArt.hpp
Target PlanFor(const std::string &tileName);   // derives TileSpriteId(tileName) inside

// proposed -- the caller supplies the convention, so the writer no longer decides it.
// `tileName` is renamed to `name` in the same pass: an object's job is not a tile.
struct Job {
  std::string name;          // the identity, for the catalog and the log
  std::string conventionId;  // what the sprite is called: tile_<slug> or <slug>
  SpriteKinds::Kind kind;    // surface vs object, for the prompt
};
Target PlanFor(const TileArtQueue::Job &job);
```

```jsonc
// the worklist, today:
{ "paletteJson": "...", "spritesDirectory": "...", "tiles": ["clay brick floor"] }

// proposed -- the flat form stays accepted, so an old worklist still reads:
{ "paletteJson": "...", "spritesDirectory": "...",
  "cells": [ { "name": "wooden crate", "conventionId": "wooden_crate", "kind": "object" } ] }
```

It is the only change in this document that can regress the working tile path, which is
why it is last (§10) and gated on the existing harnesses.

---

## 8. Sheets

A sheet is an **authoring shape, not a plumbing change**. The art side already batches N
names into one request (`TileArtQueue`), and a generation already writes one sheet with
one cell per name. So a sheet of sprites needs:

1. **A reply that carries N definitions.** The contract stays backward compatible, so
   every existing fixture and `--tile-design <file>` keeps working:

```json
// single, unchanged: a bare definition object
{ "name": "clay brick floor", "character": "b", "designerNotes": "..." }

// set, new: a list, optionally themed
{ "set": "prison", "sprites": [ { "name": "rusty bars", "designerNotes": "..." },
                                { "name": "straw pallet", "designerNotes": "..." } ] }
```

2. **Per-cell validation.** One bad sprite refuses **only that sprite**, and the refusal
   fed back to the model names its index, so a retry is targeted rather than a resend of
   the whole set.
3. **A parent action mapped to N generation requests**, one per sprite name, which the
   queue batches as it already does — subject to the cell cap (§4).
4. **A sheet review pane** showing all N sprites together, reusing the existing filmstrip
   and stage panels (`DrawFilmstripPanel`, `DrawPixelViewPanel`) rather than new UI.

### Caps and cost, stated plainly

- `SHEET_CELL_CAP` comes from §4's measurement, not from a guess. Until it is measured,
  do not raise the cap.
- A set larger than the cap is **split across several sheets**, which means
  `TileArtQueue` needs a per-request cell cap. Today it batches *everything* queued into
  one request, so this is genuine new work on the art side and belongs in this phase.
- The window shows the estimate before the first spending request of a set: *"object set,
  8 sprites → 2 image requests ≈ $0.08"*. Given the flat per-request cost, that sentence
  is exactly `ceil(N / CELL_CAP)` times the measured per-request price.

### Why the count hint earns its place

`requested` from the router (§3) is what makes an estimate possible *before* the model
has answered, which is the only time an estimate can stop a mistake rather than explain
one. The clamp is the design pressure valve: `requested` is a hint clamped to
`SHEET_CELL_CAP`, never a command that can ask for a 40-cell sheet.

**The count that matters is the one the designer actually produces**, and the two
numbers are known at different times, which the window must not blur:

| when | what is known | what is shown |
|---|---|---|
| after the router | `requested`, a guess | *"object set, about 8 sprites → if 8, 2 image requests ≈ $0.08"* |
| after the reply is validated | the real count of accepted sprites | *"6 sprites accepted → 2 image requests ≈ $0.08"* |

So the router's guess shapes the brief and gives an honest early warning, and the
**validated verdict list is what the arithmetic is computed from**. The estimate is
recomputed after validation and confirmed before the first spending request. A guess that
is wrong costs nothing: it was never the basis of the spend, only of the wording.

---

## 9. The window

The existing window becomes mode-aware, with the mode visible rather than implicit.

- **A dropdown**, defaulting to Jev's verdict. It shows the verdict's own words and its
  confidence beside it, because a classifier that is confidently wrong should be
  visibly confident rather than silently obeyed.
- **Correcting it re-runs nothing that was sent.** Changing the mode before sending
  simply builds the other prompt; changing it after a reply means a new design, and the
  transcript says which mode each exchange used.
- **A per-kind preview.** Tiles keep the glyph legend and the colour swatches. Objects
  need ASCII art rendering plus a palette, and a list of the components the definition
  asks for — `Obstacle + Storage{12}` — because that is the object's real design content
  and a preview that showed only its glyph would hide it.
- **The object prompt needs the component vocabulary**, which is the same table §5
  lists. It is read from the factory (which owns the parsers) rather than duplicated in
  the prompt file, so a component added in code appears in the prompt automatically.

`PromptRewriter`'s `RewriteBriefs::Tile()` is kind-blind today. The Improve button needs
a brief per kind, or improving an object request will push it back toward tile
vocabulary — which is precisely the failure the rewriter was added to avoid.

---

## 10. Order

Each phase ends green on its own harnesses. The sequence is chosen so that the one risky
change comes last and the free measurement comes first.

1. **Kind registry and prompts.** `SpriteKinds` (§1); split `TILEDESIGNER.md` into the
   shared reply contract plus a *one tile* / *a set of tiles* pair; write
   `data/objects/OBJECTDESIGNER.md` the same way; add `content.objectDesignerPromptFile`
   and `content.generatedObjects`; keep `--tile-design` defaulting to the tile kind and
   add `--sprite-design <kind> <file>`.
2. **Object validation and runtime registration.** `ObjectDesigner` validating through
   the real component setters (§5); `ObjectFactory::AddRuntime` and friends mirroring
   `TileRegistry`; overlay load and write for objects. Headless assertions for every
   refusal in §5 and for a round-trip through the overlay.
3. ~~**The sheet measurement (§4).** Free, re-cutting sheets already on disk. Produces
   `SHEET_CELL_CAP` and the set prompt's wording.~~ **Done, negatively (§16).** The free
   measurement cannot answer the question; a cut diagnostic and a waste-free grid shape
   were built instead, and the legibility measurement is deferred to the first real
   multi-cell set. `SHEET_CELL_CAP` stays unset, and the set prompt must not claim one.
4. ~~**The router (§3).** `SpriteRouter` over the existing `JevClassifier`, a settings block
   for endpoint/model/`minConfidence`, raw answers recorded through `SystemOneRecorder`,
   and `--sprite-route "<text>"` printing the verdict so it is verifiable with no window.~~
   **Done (§17).** Built and live; the numeric `count` question was replaced by a size class
   after real answers proved it unusable.
5. ~~**The window (§9)**, including the mode dropdown, the object preview and the per-kind
   rewrite brief.~~ **Done (§18).** The dropdown, the route button, the read-only object
   preview and the per-kind brief are in; the object preview is deliberately not an editor,
   for a reason in §18.
6. ~~**Sheets (§8)**, including the art side's per-request cell cap, and the cost
   estimate.~~ **Done (§21).** Set replies, per-entry validation, the prompt's four shapes, the
   request cap and the estimate are all in and verified.
7. ~~**Art kind plumbing (§7).** `{kind, conventionId}` through worklist →
   `TileArtQueue::Job` → `PlanFor`, and `is_surface` replaced by the explicit kind.~~ **Done
   (§19), ahead of phase 6** because the object half was silently wrong and spending money.
   Gated on `--tile-art-queue`, `--paint-memo-test` and `--art-cache-test` throughout, as
   planned.

---

## 11. Invariants

These hold across every phase and are the things to check a change against.

1. **Generated definitions go to the generated overlay, never the hand-authored
   catalog.** A tile to `data/generated/tiles`, an object to `data/generated/objects`.
2. **Generated art goes to the generated layer, never the art pack.** `PlaceSprite`
   already refuses pack writes; nothing here may weaken that.
3. **The art pack beats the generated layer.** Resolution order is unchanged.
4. **A sprite's name is the same on the write side and the read side** — `tile_<slug>` for
   tiles, `<slug>` for objects — via one convention id carried with the request.
5. **Palette-lock before the NEAREST downscale**, never after.
6. **Never block a frame.** Generation stays on the worker thread with a main-thread
   drain; classification is a detached request with a callback that always fires once.
7. **The feature works with art absent.** A created tile or object draws its glyph or its
   ASCII art, which is a legitimate rendering, not a placeholder.
8. **Name uniqueness is per kind** (§6), and a cross-kind collision warns rather than
   refuses.
9. **The model may not invent mechanics.** An object is a recombination of the ten
   registered components; anything else is refused with a reason, not stored to do
   nothing.

---

## 12. What is deliberately not being built

- **No new components.** The ten are the vocabulary. A request that needs an eleventh is a
  request for engine work, and the honest answer is the closest recombination plus a note.
- **No placement.** An accepted object joins the catalog; putting it in the world is the
  map editor's or a blueprint's job, exactly as it is for a created tile.
- **No per-message reclassification.** One verdict per design (decision 1). A
  conversation that drifts from tile to object mid-way is a new design, and the window
  says so rather than swapping prompts underneath a transcript.
- **No full rename** (decision 6).
- **No local-model path for classification.** `JevClassifier` speaks the decisions
  protocol over HTTP; the local-LoRA backend question in
  `docs/tile-art-wiring-design.md` §5 is untouched by this document.

---

## 13. Open, and honestly flagged

- **The sheet cell cap is not merely unmeasured — it is not a resolution question, and the
  free way to measure it does not exist (§16).** It will be answered by the first real
  multi-cell set, at no extra cost, and until then the set prompt must not claim a number.
- ~~**`Value` questions may be unusable.**~~ **Resolved (§17): they were, and `count` is now
  a `Choice` of size classes.** The fallback §3 pre-authorised was taken on the first five
  real requests.
- **The object prompt's quality is unknown** until it is written and run at least once.
  The tile prompt needed three corrections that were only visible from real output: a
  palette list made the model draw swatch sheets; a gutter clause on a one-cell sheet
  contradicted the full-bleed instruction; and a "rock" request came back as a solid
  object rather than a texture. Expect the object prompt to need the same treatment, and
  budget one generation request to find out.
- **Objects are more expressive than tiles, so refusals will be more common.** A tile is
  four fields; an object is a component set, parameters, ASCII art and a palette. The
  refusal text is therefore load-bearing rather than an error message, and should be
  written as instructions to the model, the way the tile refusals already are.

---

## 14. Build record: phase 1

Built and green. What it added:

| file | what |
|---|---|
| `include/SpriteKinds.hpp`, `src/SpriteKinds.cpp` | the kind table: `Kind`, `Descriptor`, `All`, `For`, `Parse`, `IdOf` |
| `data/prompts/SPRITE_CONTRACT.md` | the shared reply contract: format, refusal protocol, prohibitions |
| `data/tiles/TILEDESIGNER.md` | rewritten as the tile half, pulling in the contract |
| `data/objects/OBJECTDESIGNER.md` | the object half: the capability vocabulary and the art-block grammar |
| `TileDesigner::BuildPromptForKind` | the kind-agnostic builder, plus `PromptVocabulary` and `TileVocabulary` |
| `Settings` | `content.objectDesignerPromptFile`, `content.generatedObjects`, `content.spriteContractFile` |
| `src/main.cpp` | `--sprite-prompt <kind>`, `--sprite-design <kind> <file>` |

Two CLI modes, because the two halves of phase 1 fail differently:

- `--sprite-prompt <kind>` builds and prints the prompt, then asserts no substitution
  token survived (skipping backticked spans, since a document may legitimately *mention* a
  token in prose). This is what verifies the document work with no world and no cost.
- `--sprite-design <kind> <file>` dispatches to the kind's validator. For `object` there
  is none yet, so it builds the prompt, says the validator does not exist, and exits 3.

`--tile-design` is unchanged: still the tile-only shorthand, now routed through the same
kind table.

### Two bugs the phase found

**The component count was wrong in this document.** §5 said eight; there are ten. The
first draft stopped at the six obvious components plus `Storage` and `Evolvable`, and
missed `Requires` and `Trains` in the same function. Both are in real use in
`data/objects/`. Corrected, and the paragraph now says to read the table from the code
rather than from memory — which is the whole reason the vocabulary should be generated
from the setters in phase 2 rather than transcribed into prose.

**The prompt builder appended a second vocabulary section.** `SubstituteOrAppend` grew the
prompt when a placeholder was absent. That assumed a document lists its vocabulary only
where a placeholder sits — but a document that carries its own heading *and* the
placeholder got the vocabulary twice: once substituted, once appended underneath. It made
`--sprite-prompt tile` emit the tile list twice. Substitution is now **replace-only**
everywhere, and the degraded path for a missing prompt file is a real fallback *document*
carrying the placeholders (`kFallbackPrompt`) rather than an append. Verified: with the
object document pointed at a nonexistent path, the prompt still carries the contract and
the vocabulary, and contains no leftover token.

The second one is worth generalising: a template mechanism that both substitutes and
appends has two ways to produce output and only one of them can be checked by reading the
document. Replace-only has one.

### Verified

Twelve green, no image requests spent: `--sprite-prompt` for both kinds (asserting zero
surviving tokens), `--tile-design` over an accept and a refusal, `--sprite-design tile`,
the missing-prompt fallback, `--gaps`, `--art-cache-test`, `--paint-memo-test`,
`--tile-art-queue --tile-art-hold`, `--tile-designer-logtest`, `--tile-designer-arttest`,
and `gen_tile_art.py --cut-check`. The window's own path was checked through
`--tile-designer-logtest`: the prompt it sent is 9,660 bytes, has one reply-format
section, one vocabulary section and all 27 live tiles — and the assembled prompt contains
zero placeholder tokens.

### Left for phase 2

The object vocabulary was **empty** in the prompt at the end of phase 1, saying so out
loud: `(none: the object factory registered no components)`. That was honest rather than
broken — the document could be read and judged — but the component list is the one part of
the object prompt that must not be hand-written, so generating it from the factory's
registered components was the first task of phase 2. It is done; see §15.

---

## 15. Build record: phase 2

Built and green. What it added:

| file | what |
|---|---|
| `ObjectFactory` | `AddRuntime` / `IsRuntimeAdded` / `RuntimeNames` / `RemoveRuntimeTemplates`, and `KeyFor` |
| `ObjectFactory` | `ComponentVocabulary` / `ComponentNames` / `HasComponent` / `KnownFieldNames`, built in the same call that installs each setter |
| `include/ObjectDesigner.hpp`, `src/ObjectDesigner.cpp` | the reply handling, the verdict, the overlay write and load, and the prompt vocabulary |
| `src/Game.cpp` | the object kind's prompt vocabulary is now generated from the factory |
| `src/GameECS.cpp` | the generated object overlay is loaded at startup, after the catalog |
| `src/main.cpp` | `--sprite-design object` validates for real; `--sprite-design-overlay <dir>` makes the round trip testable |

### The wrong turn: a scratch `flecs::world`

The plan for this phase said to validate a definition "by running the same component
setters `ApplyTemplate` would, on a throwaway entity in a scratch world … so that 'the
validator accepted it' and 'the engine can spawn it' are the same statement by
construction."

That was implemented, and it is **wrong in this codebase**. flecs resolves a C++ component
type globally by name and compares the size and alignment it was first registered with. A
second world that registers `Sprite` or `Trains` on different terms than the game's world
corrupts the game's: the symptoms, in order, were

1. `terminate … Aborted` in `ecs_cpp_component_validate` — "component 'Trains' has already
   been registered with a different size or alignment", aborting the game's own
   `RegisterComponents` at startup;
2. after registering the same metadata in the scratch world, a **heap-buffer-overflow in
   `AttachSprite`** while loading the map, because the reflected layout of `Sprite` still
   did not match.

Both were reproduced from a generated object overlay. Registering identical metadata and
reusing one scratch world for the process fixed the first symptom and not the second.

So the approach was abandoned, and the reason is worth keeping: **there is no safe way to
try a template in this engine without a world, and making a second world is not a
workaround.** Validation is now a pure function of the JSON — which is also what
`TileRegistry` does with `ParseTileDef`, so this follows the established pattern rather
than departing from it.

What that means for the guarantee, stated honestly: "the validator accepted it" and "the
engine can spawn it" are no longer the same statement by construction. They are kept in
step by generating every part of the check from the same registration call that installs
the setters (`RecordComponentInfo`), and by the fact that the *only* things which can
refuse a definition are its shape — component names, parameter JSON types, required
parameters, the art block — none of which the setters interpret differently.

### What validation checks

| check | refusal? | message |
|---|---|---|
| is a JSON object | yes | `the definition is not a JSON object` |
| non-empty `name` | yes | `the definition has no non-empty "name" string…` |
| non-empty `designerNotes` | yes | `…must carry your own thoughts on it` |
| at least one component | yes | `…so the object would do nothing. … if the request is really for ground, it is a tile` |
| every component is registered | yes | `Unknown component type: Portal. …` plus `Did you mean "Storage"?` when one is close |
| parameter JSON types | yes | `"capacity" must be a number for the "Storage" component, and this definition has string instead` |
| required parameters present | yes | `"Placeable" needs "placement.becomes" (a string), and the definition does not have it` |
| key is hand-authored | yes | `…a designer may not replace hand-authored content` |
| art rows ragged | no, warns | `…will be padded and the drawing lopsided` |
| palette misses an art glyph | no, warns | `…those cells draw white instead of the colour you intended` |
| key the engine does not read | no, warns | `"lumens" is not a field the engine reads…` |
| name also used by a tile | no, warns | `…the two are separate namespaces and this is allowed…` |

The parameter-type table exists because the setters are lenient: `capacity` is read with
`value("capacity", 10)`, so `"capacity": "lots"` silently becomes 10, and the places that
index directly (`Placeable`) throw a raw nlohmann `type_error` that reaches a user as
`[json.exception.type_error.302]`. Checking the JSON type is duplicated work on purpose,
bounded to the type and never the semantics, so it cannot disagree with a setter about
what a value *means*.

### Two more bugs found

**Re-designing an existing runtime object was refused.** `AddRuntime` refused any key that
already existed, so "same crate, taller" — the most ordinary thing to want — failed with
`the key 'wooden_crate' already exists`. Now a key that `IsRuntimeAdded` is replaced and
the verdict reports `REVISED` rather than `ACCEPTED`; a hand-authored key is still refused,
which is the only case the rule was for.

**The overlay reported only into the debug log, which nothing prints.** A refused or
malformed overlay file produced no line anywhere a headless run could see — the run
silently loaded nothing. `ObjectDesigner` now reports through both the log and stderr,
because a message only one audience can see is the same bug as no message for the other.

Also found: `--sprite-prompt object` still built an empty vocabulary after `Game` had been
fixed, because the CLI assembled its own. Both now read from one function. That is the
second time in two phases that two paths assembling the same thing drifted; the rule that
falls out is that the vocabulary is built in exactly one place per kind.

### Verified

Twelve green, no image requests spent: `--sprite-prompt` for both kinds, `--tile-design`,
`--sprite-design` for both kinds, `--gaps`, `--art-cache-test`, `--paint-memo-test`,
`--tile-art-queue --tile-art-hold`, `--tile-designer-logtest`, `--tile-designer-arttest`,
and `gen_tile_art.py --cut-check`. Eleven object fixtures cover three accepts and eight
refusals. The end-to-end path was checked through a **real boot**: the designer writes
`apple_sapling.json` to a disposable overlay, the game loads it at startup and runs to the
frame deadline with ASan clean.

> **Correction (phase 3).** This section originally quoted
> `Object overlay: loaded 1 created object(s)` as the evidence for "the game loads it".
> That line was not evidence — `DebugLog` is an in-memory ring buffer that nothing prints,
> so no boot log could contain it. See §16: `--object-check` now makes the claim testable,
> and the boot was re-verified with it.

### Left for phase 3

The sheet measurement, which is free and unblocks the set prompt. Nothing in phases 1–2
depends on it.

---

## 16. Build record: phase 3, and a measurement that cannot be made

Phase 3 was planned as the free measurement that sets `SHEET_CELL_CAP` before any prompt
depends on it. **The measurement as specified is impossible**, and the honest outcome of
this phase is that finding plus the two things that *were* free.

### The correction

The plan: re-cut an already-paid sheet at 4 and at 16 cells and compare sprites per cell.

That does not measure what it claims. A sheet holding four subjects cut on a 16-cell grid
produces sixteen quarter-sprites, not sixteen sprites. What changes as N grows is the
model's willingness to draw N distinct subjects in N cells — the repeated-motif collapse
the `puddle` tiles already demonstrated — and that is a property of a *generation*, not of
a crop.

The arithmetic that was supposed to justify the cap is also wrong, and the real numbers
say so:

| generation | sheet the model returned | cell after cut | source px per output px |
|---|---|---|---|
| 1 cell | 1024×1024 | 1024 | **32.0** |
| 4 cells | 400×400 | 200 | **6.2** |

The model picks the sheet size and does not scale it with the cell count. So the 4-cell
batch — the one that produced four usable surface tiles — had *five times less* source
detail than the 1-cell sprite that went wrong, and the 1-cell failure was the wash stage
eating 73% of the tile, not its resolution. A resolution threshold derived from these two
points would classify the good case as bad and the bad case as good.

The generalisable lesson: **"we can measure it for free from data we already have" is a
claim to check before it is a plan.** Re-cutting was free and answered a different
question, and two phases were sequenced behind an answer that was never available.

### What was built

**A diagnostic, not a gate.** The cut stage now prints the source-to-output ratio per
cell (`1024x1024 -> 32x32 (32.0 source px per output px)`). It is deliberately *not* used
to refuse a cut, because the data above does not support a threshold. It is the number to
watch when the first real multi-cell set is generated, and it explains a soft sprite after
the fact.

Getting the number right took three attempts, which is its own lesson about "obvious"
statistics: the first printed an area ratio (1024.0 — the square), the second multiplied
the two axis ratios (also 1024.0, and alarming for no reason), and only the third was the
per-axis scale a person can reason about (32.0). One side, one number.

**A grid shape that wastes nothing.** `--from-worklist` chose `ceil(sqrt(N))` columns,
which turns eight tiles into a 3×3 grid with one cell empty — a ninth of a paid-for sheet
spent on nothing. It now picks the widest divisor-based shape whose rows are all full
(`grid_columns`): eight becomes 4×2, twelve 4×3, ten 5×2. Measured across 1–16, wasted
cells are now zero in every case rather than 0–2.

### Where the measurement moved

Per the owner's decision, the legibility question is **deferred to the first real
multi-cell set request**, where a genuine 9- or 16-cell sheet arrives for money that was
being spent anyway. At that point the diagnostic above reports the ratio, and the sheet
itself answers whether the model honoured one-subject-per-cell.

Until then `SHEET_CELL_CAP` is not a constant and the set prompt must not claim one. What
the set prompt may say is the part that is known: keep the number of cells small enough to
stay legible, and the wrapper splits a set across requests rather than asking for one
enormous sheet.

### Verified

`--cut-check` still green. The diagnostic was verified against both real sheets, producing
32.0 and 6.2 — the values in the table above, which is what makes the table a measurement
rather than a restatement. `grid_columns` was checked across 1–16 for zero wasted cells,
and the worklist path was verified to emit `--cols 4` for eight tiles.

### A phase-2 claim that turned out to be unverifiable, and is now verified

Phase 2 recorded that "the game loads the overlay at startup (`Object overlay: loaded 1
created object(s)`)". That line was never actually evidence: `GameECS` reports through
`DebugLog`, and `DebugLog` is an in-memory ring buffer whose `Log` method only appends —
`src/DebugLog.cpp` has no print or flush anywhere. Nothing shows it except the in-game
window, so no boot log could ever have contained it, and the claim was really "the code
path ran", not "the object is there".

`--object-check <key>` closes that gap: it boots the real game and asks the **running
factory** whether the key resolved, and whether it came from the overlay or the catalog.

```
$ AIRogue --settings <overlay settings> --object-check apple_sapling
object:     apple_sapling
  loaded as "apple sapling" from the generated overlay
```

Checked three ways so the test cannot pass vacuously: the designed object reports *the
generated overlay*, the hand-authored `anvil` reports *the hand-authored catalog* (so the
two sources are distinguishable, not both reported as "loaded"), and an absent key fails
and prints what the factory does hold.

The generalisable part: a log line is not a verification, and a log sink nothing prints to
is worse than none, because it reads like evidence.

---

## 17. Build record: phase 4, the router

Built and green, and live — the first phase whose code path has been run against the real
decision model rather than a stub.

| file | what |
|---|---|
| `include/SpriteRouter.hpp`, `src/SpriteRouter.cpp` | the three questions, the verdict, the parse, and `--sprite-route` |
| `Settings` | `system1.routeRequests`, `system1.minConfidence`, `system1.timeoutSeconds` |
| `SpriteRouter::ConfigFromSettings` | the endpoint/model/key resolution, now shared with `--systemone-test` |

`ConfigFromSettings` is the one piece lifted out of `main.cpp`. The endpoint rule is subtle
— a `model.endpoint` is a *chat* endpoint, and posting a decisions body to
`localhost:11434/api/generate` is how `--systemone-test` originally failed — so two copies
would have meant one of them getting it wrong.

### The `count` question was wrong, and five requests proved it

The design asked for three questions: kind (`Choice`), shape (`Choice`), count (`Value`,
levels 1–8). The first five real requests:

| request | kind | shape | count the model gave |
|---|---|---|---|
| "Create a rotting wooden floor tile" | tile ✓ | single ✓ | 1 ✓ |
| "a wooden crate that stores things" | object ✓ | single ✓ | 1 ✓ |
| "a stone wall" | object ✓ (corrected) | single ✓ | 1 ✓ |
| "a prison related sprite sheet" | object ✓ | **set** ✓ | **1** ✗ |
| "some shelves and barrels for a cellar" | object ✓ | **set** ✓ | **2** ✗ |

`kind` was right on all five *as the test was written*, including the case the design was written
for ("a crate that stores things" contains no tile-or-object word and is an object only because the
classifier is told `Storage` exists). **One expectation was wrong rather than the model:** the
original run recorded "a stone wall" as `tile`, which is the taxonomy error this correction removes.
A wall is an object, never a tile, so `object` is both the required answer and the corrected
expectation; the row above says so. `shape` was right on all five. `count` was right only where the
answer was 1, and never left the bottom of its scale — 0.03, 0.04, 1.1, 0.38 on a 0–7
index.

The diagnosis is not "the model is bad". It is that **the question was wrong**: a request
phrased "a prison sheet" does not name a number, so asking for a weighted level index asks
the model to invent precision it does not have. Asked for a *class* it answers well.

So `count` is now `Choice` of `small` / `medium` / `large`, mapped to 4 / 8 / 12 sprites.
This is exactly the fallback §3 pre-authorised — *"demote `count` to a `Choice` of size
buckets … and let the set prompt pick a number inside the bucket"* — so the decision had
already been made; the data only triggered it. Re-measured on the same requests:

| request | before | after |
|---|---|---|
| "a prison related sprite sheet" | object, a set of 1 | object, a **large** set (~12) |
| "some shelves and barrels for a cellar" | object, a set of 2 | object, a **medium** set (~8) |

Two things fell out of the change:

- **`size` and `shape` are asked separately, not inferred from each other**, because a
  single sprite is one sprite whatever the size answer says, and "a set of one" is not a
  shape anyone means. `Parse` clears the size class when the shape is `single`.
- **`large` (12) is a guess** and the only one of the three that is. `small` and `medium`
  are the numbers §3 already proposed; twelve is above them and unmeasured, and it is the
  value to change once a real large set has been generated. Recorded here so it is not
  mistaken for a measurement.

### A correction to a comment, from the same data

The header originally carried the `SystemOneRecorder.h` warning as "`Choice` answers carry
no confidence". The raw answers disproved it: hosted Jev reported confidence on `Choice`
answers throughout — kind 0.98 and 0.74, shape 0.54, size 0.20 — so the comment would have
taught the next reader to treat a real signal as structurally impossible. The underlying
rule still holds (`Answer::confidence` *is* absent on some answer types, and a missing one
must not be read as 1), so the comment now says that instead, and records the counter-example.

A related bug the raw output caught: `shapeConfidence` and `sizeConfidence` were declared
and never assigned, so the report printed `0` while the raw answers printed `0.54`. The
mode that prints both is what made the discrepancy visible — which is the argument for
printing raw answers rather than a summary.

### What the verdict is used for, and what it is not

The router selects the **prompt** and gives the window an early estimate. It does not decide
the count of anything: the model that designs the set produces as many sprites as it
produces, and the arithmetic that gates spending is computed from the validated list
(§8). A wrong `size` therefore costs wording, never money.

### Verified

Live, against `~typesafe/jev-latest`: four requests covering both kinds, both shapes and two
size classes, all parsed correctly. The raw answers are printed unconditionally by
`--sprite-route`, which is how the count failure and the confidence bug were both found.
`--systemone-test` still passes on the shared config. The rest of the suite (twelve checks
from phases 1–3) is unchanged.

---

## 18. Build record: phase 5, the window

Built and green. The window is now driven by the kind table rather than by one kind:

| change | where |
|---|---|
| the mode dropdown, with the route button beside it | `TileDesignerWindow::Draw` |
| the live classifier the route button needs | `Game::systemOneClassifier`, built in `Game::Init` |
| routing off the main thread, applied on it | `RequestRoute` / `ApplyRouteVerdict` / the latch in `Draw` |
| per-kind inspection and preview | `InspectFinishedExchange`, `DrawObjectPreview` |
| the per-kind rewrite brief | `RewriteBriefs::Object`, `RefreshRewriteBrief` |
| `ObjectDesigner::ApplyReply` | so the window and `--sprite-design object` accept the same reply forms |
| `--sprite-designer-objecttest` | the canned conversation with an object reply |

### `designMode` is not `mode`, and the difference is a real bug avoided

A reply is **inspected on a later frame than it was requested**, so the kind is latched into
`designMode` when the request is sent (`SendUserMessage`) and inspection uses that, not the
dropdown. Without it, flipping the dropdown while a request is in flight makes the reply be
judged against the other kind's grammar — and object JSON put through the tile validator is
refused for having no glyph and no colour, which is a baffling way to be told about a
dropdown. The same distinction decides **which overlay the definition is written to**:
`ModeOverlayDirectoryFor(designMode)`, not the current mode, or an object can be written
into `data/generated/tiles`.

### The object preview is read-only, and that is a finding rather than a shortcut

The tile preview is a live editor: `edited` points into `TileRegistry`, the pane edits
through it, and `TouchRuntime` re-derives the tile. That works because `TileRegistry` stores
its definitions behind `unique_ptr`s, so a pointer stays valid for the run — a deliberate
design of the runtime-add work.

`ObjectFactory` stores templates **by value in an `unordered_map`**, and its own comment
already warns that `GetTemplates()` must not be held across a mutation. A pointer into that
map is invalidated by the next runtime add, so an object editor built on the tile pattern
would be a use-after-free waiting for the second object of a session. The preview therefore
holds a **copy** of the definition (`editedObject`) and is read-only, and the header says so
at the member rather than leaving the asymmetry to be discovered.

Editing an object still needs to be possible — a wrong `capacity` should not require a
re-prompt — so the honest next step is a registry that hands out stable handles, the same
fix the tile side already has. That is not phase 5's job and is recorded here rather than
half-done.

### What the mode dropdown and the route button actually do

- The dropdown shows the current kind and switches it by hand. Switching **rebuilds the
  system prompt** and **re-applies the rewrite brief**, because both are per-kind; the
  transcript is left alone, since the conversation so far is still readable and the next
  request is what changes.
- The route button reads the text currently in the input box, asks the classifier, and
  switches the dropdown to match. It is **disabled with a tooltip explaining why** when no
  classifier is configured, rather than hidden: an absent control explains nothing.
- The verdict is shown with its confidence and the model's own words — "a classifier that
  is confidently wrong should be visibly confident rather than silently obeyed" (§9).
- An **unanswered** verdict leaves the mode where it is and says so. It never guesses the
  more likely kind.
- The answer arrives on a worker thread and is **latched**, then applied by `Draw` on a
  later frame. Applying it in the callback would switch the mode and rebuild a prompt from a
  background thread while the frame is mid-draw.

### Two things the phase found by looking

**The session log header was tile-specific.** It still said `TILE DESIGNER SESSION`, logged
`generatedTiles` as the overlay, and dumped `TILEDESIGNER.md` as the prompt file — so an
object session's log named the wrong document, which is exactly the record a prompt
disagreement is diagnosed from. Now the header names the kind, its overlay and its prompt
file. Found by reading the log of the first object self-test rather than by assuming the
header was generic.

**`ObjectDesigner` had no `ApplyReply`.** It had `ApplyObject` (parsed JSON) and `ApplyFile`
(a path), and the window needed the third form — a model reply with reasoning and a fence.
`ApplyFile` now routes through `ApplyReply`, so a file the headless mode accepts and a reply
the window accepts go through one implementation; before, a bare object and a fenced one
were handled in two places.

### Verified

Seventeen green: `--sprite-prompt` for both kinds, `--rewrite` for both sprite briefs,
`--tile-design`, `--sprite-design object`, `--object-check` (overlay and catalog),
`--gaps`, `--art-cache-test`, `--paint-memo-test`, `--tile-art-queue --tile-art-hold`,
`--tile-designer-logtest`, `--tile-designer-arttest`, `--sprite-route`,
`gen_tile_art.py --cut-check`, and a real boot with an object overlay.

The object half of the window was verified through `--sprite-designer-objecttest`, which runs
the canned conversation with an object reply: the object is accepted, written to
`data/generated/objects/self_test_post.json`, removed by the test, and the session log shows
`kind: object`, `overlay: data/generated/objects` and `OBJECTDESIGNER.md` as the prompt file.
Adding `--tile-art-hold 40` then **drew the window for forty real frames with the object
preview rendered**, under ASan, clean — which is what makes "the preview draws" a claim
rather than an intention, the same reasoning as the art panels' hold phase.

### The headless surface, in one place

Seven modes now exist, and they are the reason every phase above could be verified without a
window. Collected here because until now they were only in `main.cpp`'s argument comment:

| mode | what it proves | cost |
|---|---|---|
| `--sprite-prompt <kind>` | the prompt assembles, with no substitution token surviving | free |
| `--sprite-design <kind> <reply>` | one reply is validated or refused, by kind | free |
| `--sprite-design-overlay <dir>` | an accepted definition round-trips through the overlay | free |
| `--sprite-route "<text>"` | one live classification, printed with the raw answers | one decision call |
| `--object-check <key>` | an object is in the **running game's** factory, and from which layer | free |
| `--sprite-designer-objecttest` | the window's object inspection path, canned reply | free |
| `--tile-designer-logtest` / `--tile-designer-arttest` | the window's tile path and the auto-art request | free |

`--tile-art-hold <n>` applies to the last three and **draws the window for n real frames**,
which is what makes "the panel drew" a claim rather than an intention.

### Left for phase 6

Sheets. The art side's per-request cell cap, the set prompt, and the cost estimate — and the
legibility measurement that phase 3 deferred, which now rides along with the first real set.

---

## 19. Build record: phase 7, the art kind plumbing

Built and green, **out of order** — before phase 6, because phase 7 turned out not to be the
polish §10 expected but a live money bug.

### The bug, stated precisely

`TileArt::PlanFor(tileName)` derived the sprite's filename internally:
`conventionId = TileSpriteId(tileName)`, which always prefixes `tile_`. That was correct
while every sprite was a tile. `ObjectFactory` resolves an object's sprite through
`AttachSprite(obj, explicitSprite, type, ...)` where `type` is the template key — the *bare*
key, no prefix. So:

```
request art for "apple sapling"
  written:  packs/generated/.../tile_apple_sapling.png
  looked up by the object:  packs/generated/.../apple_sapling.png
```

Every object sprite the designer asked for was paid for and then never seen. Nothing
reported it: the queue said `Done`, the file existed, the path was logged. It surfaced only
because the window's auto-art request is shared by both kinds and I checked the naming
instead of assuming the tile path generalised.

### The fix, and where the rule now lives

`SpriteKinds::ConventionIdFor(kind, name)` is the single authority:

| kind | id | from |
|---|---|---|
| tile | `tile_<slug>` | `TileSpriteId` |
| object | `<slug>` | `ObjectFactory::KeyFor` |

`TileArt::PlanFor` now takes a `Request{kind, name}` and calls it, so the writer and the
reader cannot disagree. `PlanForTile(name)` remains as the shorthand for the tile-only
callers (`--tile-art`, the hot-swap self-test, `SwapIn`), which is what keeps the change
from rippling through those modes.

The `kind` travels: `Game::RequestTileArt` → `TileArtQueue::Request(kind, name)` →
`Job{kind, conventionId}` → worklist → generator. `Game::KindForSpriteName` derives it from
the live registries at request time.

### Two things found while doing it, both real

**The registry lookup compared a display name to a template key.** `KindForSpriteName`
first looked for the name in `GetTemplates()`, which is keyed `apple_sapling` while every
caller passes `"apple sapling"`. It answered "tile" for every object. Fixed by trying the
key and then scanning display names -- and the bug was caught because the first object
through the queue still came out as `tile_apple_sapling.png`, which is what the test existed
to see.

**`is_surface` guessed from words, and objects broke the guess.** It scored names and
descriptions against `SURFACE_WORDS` (`"wall"`, `"stone"`, `"roof"`, …), which is the old
taxonomy in code form: `wall` and `roof` are *object* words and should never select a tile. An
object called *stone wall* would have been sent the tile prompt's full-bleed surface wording; a
ground tile whose name missed every word would have been asked for as a centred object. The kind now
decides it, the word list is confined to a fallback for a worklist that never recorded a kind, and
the check is verified on a mixed sheet:

```
stone wall          (object) -> "ONE SINGLE complete object, centred in the cell"
herringbone parquet (tile)   -> "a full-bleed SURFACE TEXTURE covering the whole cell"
```

Note the second: a *ground tile* whose name contains no surface word, which the old heuristic
would have got wrong in the other direction. "Archway" was used as the tile example in an earlier
revision; an archway is an object, so that example has been replaced.

A third, smaller one came out of the same work: the prompt's framing sentence — *"The cell
is a piece of ground, not a picture of an object on a shelf"* — was actively wrong for an
object, and would have told the model to put a crate on a floor. It is now three spellings:
all-tiles (ground), all-objects (*"no floor, no table, no shelf and no ground plane under
it"*), and mixed (both clauses).

### The naming rule is written once, and the worklist is how

The generator used to re-derive the filename in Python (`slugify`, which hardcoded the
`tile_` prefix and said in its own comment that it was a duplicate kept for convenience).
Now the **engine supplies the convention id** in the worklist and the tool uses it verbatim;
`slugify` survives only as a fallback for a worklist written by an older build. The
`--from-cells` hand-off exists because `run_from_worklist` re-invokes the script as a child
process, and passing bare names through `argv` silently lost the kind — which is how the
first attempt at this fix produced a correct worklist and a wrongly named file.

### Harnesses that had to grow up

Three tile-shaped assumptions in the harnesses themselves, each found by running the object
path through them:

- `--tile-art-queue` required `swappedCells > 0`. An object is a spawned entity, not a
  painted cell, so zero is correct for it and a failure for a tile. Now branched on the
  job's kind, and reported rather than silently passed.
- The same for the later `cellsAfter == 0` check.
- `--paint-memo-test` required a sprite to exist and refused otherwise, so it failed for a
  reason unrelated to the cache whenever another mode had cleared the generated layer. It
  now creates the sprite it needs, and accepts a zero-art baseline -- which is the stronger
  case anyway, since a sprite created after the boot leaves cells on their glyphs.

### Verified

Seventeen green, including both kinds end to end through the queue:
`job path: .../apple_sapling.png` for the object, `.../tile_marble_floor.png` with
`64 swapped` for the tile. The worklist is checked directly:

```json
[{ "conventionId": "apple_sapling", "kind": "object", "name": "apple sapling" }]
```

and the file on disk matches it. `--cut-check` is still green, the tile queue still swaps
cells, and the prompt framings were checked for all three combinations on a mixed sheet.

Phase 7 turned out to be two bugs short of done, both found by the owner actually using the
feature. They are recorded in §20 rather than folded in here, because neither was visible
from the tests phase 7 shipped with -- which is the point worth keeping.

---

## 20. Two bugs found by using the feature

Both were reported after phase 7 shipped green: an object was created with art, the art was
not loaded, and the object covered the tiles it stood on. Neither was reachable from the
tests phase 7 shipped with, and that is the honest lesson of this section — the harnesses
covered *naming*, which is what phase 7 was about, and the two failures were about *loading*
and about *what the pixels contain*.

### One: the art was never loaded

`Game::HandleTileArtGenerated` refreshed the world by calling `RefreshTileVisuals`, which
walks **map cells** looking for tiles that draw the named tile. An object is not painted onto
a cell; it is an entity spawned from a template. So for an object the refresh found nothing,
returned zero, and logged the same "the tile is not on the map yet" that a legitimately
unpainted tile produces. Nothing was wrong with the file, the path or the queue — nothing was
looking at entities.

`Game::RefreshObjectVisuals(templateKey)` is the missing half: it collects live entities whose
`ItemType::id` matches, forgets the memoised sprite path, removes `Sprite` and re-attaches. It
matches on `ItemType::id` rather than `MapAuthored::templateKey` because `ItemType` is
restamped when a template is re-applied — an Evolvable object that grew should get the *grown*
template's art.

`HandleTileArtGenerated` now branches on `job.kind`, and reports in the right unit
("entity(ies)" versus "cell(s)") with the right explanation for zero.

**`--object-art-swap <key>` is the harness for it**, and it had to be written carefully to
mean anything: it moves the sprite aside, spawns an entity from the template, asserts the
entity really is on its ASCII art, writes a sprite, refreshes, and asserts the `Sprite`
component arrived. Its first version failed with "the entity already had a sprite" — because
an earlier mode had left a file behind, so the *spawn* resolved it and there was no arrival to
observe. A test that depends on the absence of leftovers from another test is not a test.

### Two: the art had an opaque background

Measured on the sprite the owner generated: **1024 of 1024 pixels opaque**, with a solid grey
corner. In the world that rectangle covered the tiles underneath.

Two causes, and the first explains the second:

1. **The prompt said so.** The sentence *"The subject's background fills its whole square cell
   edge to edge, with no transparency"* was global — correct for a tile, whose whole job is to
   fill its cell, and exactly wrong for an object, which sits *on* the ground. It is now split
   by kind: an object cell asks for one flat magenta background reaching every edge, the
   subject centred and **not touching any edge** (an object bleeding off the cell would be cut
   in half by the key), and no ground or shadow behind it.
2. **The cut stage assumed a surface.** It washes the gutter, fills the fringe from the
   neighbouring art and produces an opaque, edge-to-edge image — deliberately, because a tile's
   edge must be the texture. `cut_object_cell` is now a separate path and a cut-out rather than
   a surface: key the background, trim to what remains, pad back out to a centred square so the
   object keeps its proportions, NEAREST-resize, then palette-lock.

The palette lock has a trap worth recording. `-remap` operates on colour and knows nothing
about transparency, so remapping a keyed image directly paints the transparent area with the
nearest palette colour — undoing the key. The alpha is therefore extracted first and
composited back after the remap:

```
magick squared.png -alpha extract alpha.png
magick squared.png -alpha off   rgb.png
magick rgb.png -remap palette.png locked_rgb.png
magick locked_rgb.png alpha.png -compose CopyOpacity -composite destination.png
```

Verified on a synthetic sheet built to look like what the new prompt asks for: 540/1024
pixels opaque, corner `srgba(0,0,0,0)`, **0.0% off-palette**, aspect preserved.

### The guard, and the asset this cannot rescue

A key that removes nearly everything is a failure, not a result: it happens when the subject
is close in colour to its own background. The owner's existing sheet is exactly that — 91.7%
of it is a uniform grey and the object is grey stone on it, so a border-connected flood
removed **100%** of the cell. A fully transparent sprite is worse than an uncut one, because it
hides the object's glyph fallback and reads as the object vanishing. So if fewer than 2% of
pixels survive the key, the uncut cell is used and the reason is printed.

That sheet was generated under the old prompt and **cannot be salvaged**: it is not magenta, and
its object is the same colour as its background. It needs one more image request, with the
prompt that now asks for a magenta background.

---

## 21. Build record: phase 6, sheets

Built and green. A set is now a first-class thing to ask for, validate, name and generate art
for.

| change | where |
|---|---|
| the two shape sections per kind document, and the builder that selects one | `TILEDESIGNER.md`, `OBJECTDESIGNER.md`, `TileDesigner::SelectShape` |
| the reply envelope, shared by both kinds | `include/SpriteReply.hpp`, `src/SpriteReply.cpp` |
| per-entry validation that does not roll back | `TileDesigner::ApplySetReply`, `ObjectDesigner::ApplySetReply` |
| the per-request cell cap | `TileArtQueue::Config::maxCellsPerRequest`, `StartWorker` |
| the shape checkbox, per-entry transcript, and the cost estimate | `TileDesignerWindow` |
| `--sprite-prompt-shape`, `--tile-art-split`, `--sprite-designer-settest` | `main.cpp` |

### The four prompts come from two documents, by stripping

§2 chose "two files with two sections each" over four documents that drift apart. Implemented
as comment fences — `<!--%%SINGLE%%-->`, `<!--%%SET%%-->` — which `SelectShape` strips: the
sections of the shape *not* asked for are removed entirely, and the fences of the one that
applies are removed but its content kept. Reading a document on its own therefore shows no
scaffolding, and the model never sees a fence.

An unmarked document passes through unchanged, so a hand-written prompt file still works and
simply has no set instructions.

Verified for all four combinations, which is cheap because it is a prompt dump:

| | `one` | `set` |
|---|---|---|
| tile | 9,774 bytes, no set section | 11,882 bytes, set section, no "define exactly one" |
| object | 10,811 bytes | 13,143 bytes |

No fences and no substitution tokens survive in any of them.

### A refused entry does not roll back an accepted one

This is the decision that shapes the code, and it is worth stating as a behaviour: a set of six
where entry 2 has no `designerNotes` leaves **five** tiles in the catalog and tells the model
to fix entry 2. The alternative — all-or-nothing — would discard five good definitions because
of the sixth, which is the wrong trade for a whole set of paid-for designs.

`SetApplyResult` therefore separates "could the reply be read" (`ok`) from "how many entries
passed" (`accepted` / `refused`), and the refusal text is prefixed with the **index**:
`entry 1: the definition has no non-empty "designerNotes" string…`.

Three envelope failures are refused before any entry is tried, because each is fixable on the
next attempt if named: `sprites` that is not an array, an empty array, and an entry that is not
an object. There is also a hard ceiling of 24 entries — far above the prompt's "four to eight"
— because every entry costs an overlay file, a catalog entry and a generation cell, and a model
that sends forty has not produced something a person would place.

### The cell cap is a setting, not a constant

§4 proposed a `SHEET_CELL_CAP` constant; §16 concluded the free way to measure it does not
exist. The resolution: **it is a setting**, `art.generation.maxCellsPerRequest`, defaulting to
4 — the largest sheet this project has actually measured as usable — precisely so it can be
raised the moment a real set has been looked at.

The prompt deliberately claims no number, because a cell count is not a legibility guarantee.
The *split* is mechanical and needs a number, and a setting is the honest place for a number
whose right value is unknown.

A set larger than the cap is split across requests rather than refused: jobs past the cap stay
`Queued` and the next batch window takes them, so ten sprites at a cap of four become three
requests. `Update` already refused to run a second worker, so the batches sequence instead of
overlapping.

Verified at three caps, which is what proves the setting is read rather than the constant
assumed: cap 4 with 10 sprites → 3 requests; cap 2 with 5 → 3; cap 8 with 6 → 1. Every sprite
was generated in each case.

### The estimate is arithmetic, not a promise

The accept path prints:

```
ART REQUESTED (automatic): 2 sprite(s) will take 1 image request(s) at a cap of 4 per
request (~$0.04)
```

`ceil(n / cap)` times the measured per-request price. It is shown for a single sprite too,
where it is always one request — because the point is that the cost of a set is visible
*before* it is spent, and a line that only appeared for sets would make a set feel exceptional.
§8 argued this is the only feature that spends money by default, which is why the figure is
stated in the transcript rather than buried in the log.

### Three bugs the phase found in its own tests

All three were cases of a test running the wrong path convincingly, which is worse than a test
that fails:

1. **The set test validated object JSON through the tile validator.** `--sprite-designer-settest`
   inherited the default kind of tile, so every entry was refused for having a `components`
   field the tile grammar does not know — and the run still exited 0 with "inspected 1
   exchange". It now sets the object kind, because its fixture is object-shaped.
2. **The self-test's fabricated exchange never latched the shape.** `SendUserMessage` latches
   `wasSet` for a real request; the canned conversation builds its `Exchange` directly and did
   not, so the set reply was judged as a single definition, refused for having no top-level
   `name`, and retried. A `wasSet` this code did not set is a test that exercises the refusal
   path while claiming to exercise the accept path.
3. **A retry did not inherit the shape.** A retried set was re-judged as a single, refused
   again, and burned the whole attempt budget on a question the model had already answered.
   `retry.wasSet = finished.wasSet`.

A fourth, smaller one: the object set branch set `objectOverlayPath` and not `savedPath`, so a
set that had written its files logged `ADDED: … (not saved)`.

### Verified

Twenty-one green, including the three new ones. End to end through the window:

```
ADDED SET 'self test set': 2 of 3 definition(s)
  [0] ACCEPTED 'self test grate'
  [1] REFUSED  entry 1: the definition has no non-empty "designerNotes" string. …
  [2] ACCEPTED 'self test bucket'
ART REQUESTED (automatic): 2 sprite(s) will take 1 image request(s) at a cap of 4 …
  queued: self test grate
  queued: self test bucket
```

Both accepted entries were written to the overlay and removed by the test; the refused one was
never written, which is what makes "one bad entry does not discard the rest" observable.

### One unexplained observation, recorded rather than dismissed

The very first run of `--sprite-designer-settest` reported an ASan **heap-use-after-free** on a
worker thread, with the stack truncated and no symbolised frame. It has not reproduced in
**eight** subsequent runs. The run coincided with a stale `AIRogue` process still holding the
REST port, so two instances were competing over `debug_windows_state.json` and the shared work
directory; that is the leading explanation and it is not a satisfying one.

It is recorded here instead of being written off, because "it went away" is not a diagnosis. If
it returns, the thing to look at first is shutdown ordering between `Shutdown()`,
`TileArtQueue::Stop()` and a worker that receives a completion for a batch the queue no longer
owns — which is the only lifetime boundary the set path exercises more heavily than the single
one.

---

## 22. The object was opaque for two independent reasons

Reported after §20 was closed as fixed:

> currently objects backgrounds generated in the sprite designer are not transparent so we
> can't see the tile's underneath them

Both causes were real, and fixing either one alone leaves the object as a solid rectangle. They
are recorded together because the second one only became visible once the first was fixed: with
the plate gone, the sprite's *own* un-keyed background was what covered the tile.

### One: the renderer painted a plate behind every sprite

`Game::DrawEntityVisual` did this, in this order, for every visual:

1. fill the cell with `DrawAscii::backgroundColor`;
2. draw the `Sprite` over it with `DrawTexturePro(..., WHITE)`.

The sprite's own alpha was honoured, but its **transparent pixels revealed the plate**, not the
map tile underneath. Generated objects carry an opaque `backgroundColor` because the designer
picked one for the *glyph fallback* — `clay_jug` `[100,70,50,255]`, `wicker_basket`
`[90,70,50,255]` — so a fully cut-out sprite still arrived in the world as a brown square.

The plate is **not** vestigial: it is the backdrop for the glyph and the ASCII-art fallback, both
of which draw their own pixels and no background. So the fix is not "delete the plate", it is
"draw it only when nothing covered the cell". The sprite is now resolved first, and the plate and
the glyph/art are drawn only when `drewSprite` is false — which is a different condition from
"the entity has a `Sprite` component", because a `Sprite` whose texture was evicted falls through
to the art, and that art needs the plate.

The same rule is now applied to the map editor's object palette (`DebugWindows.cpp`), whose
promise is "what placing the object will look like": a palette button behind a sprite is
transparent too, so a cut-out object shows the window rather than a plate the world would not
draw.

### Two: the magenta key was being skipped, so the sprite had no transparency at all

The prompt asks the model for a flat magenta cell background (`background_clause`, objects only)
and `cut_object_cell` keys that colour out. It was not keying it out, because the guard that
decides "is this magenta?" rejected what the model actually emitted:

```python
GUTTER_MIN_CHANNEL = 140   # both R and B
GUTTER_MAX_GREEN = 120
return r >= GUTTER_MIN_CHANNEL and b >= GUTTER_MIN_CHANNEL and g <= GUTTER_MAX_GREEN
```

The real generation log said:

```
warning: cell 0: the background is not magenta ((215, 52, 135)),
         so it cannot be removed and the sprite will be opaque
```

Red 215 passes, green 52 passes, **blue 135 fails `>= 140` by five**. The key was skipped, the
uncut cell was used, and the palette lock mapped the near-magenta to the nearest palette colour —
a red. That threshold was tuned to a single earlier sample, `srgb(205,50,141)`, whose blue was
**one unit** over the floor. A guard that passes by one unit is not a guard.

`looks_like_magenta` now tests what magenta *is* — red and blue both far above a suppressed
green, and far enough from grey — instead of absolute channels:

```python
def looks_like_magenta(pixel):
    r, g, b = pixel[0], pixel[1], pixel[2]
    return r > 110 and b > 100 and g * 2 < min(r, b)
```

`--cut-check` asserts the three magentas the model has actually emitted — `(255,0,255)`,
`(205,50,141)` and the `(215,52,135)` that broke the old floors — and ten texture colours that
must not match, including the palette red `(180,47,47)` an un-keyed background becomes. Reverting
the predicate to the old absolute floor makes it fail, which is the point of keeping that sample
in the test.

### What was rescued for free, and what was not

`data/generated/work/tile_art_sheet.png` still held the **wicker basket** batch, so that sprite
was re-cut with `--sheet` at no cost: 900/1024 opaque → **544/1024**, corner `srgba(3,3,6,0)`,
a real basket silhouette. The rest of the batch's sheets are gone — each generation overwrites
that one file — so the other generated objects are still opaque *assets*, not a rendering bug:

| sprite | opaque | verdict |
|---|---|---|
| `wicker_basket.png` | 544/1024 | re-cut from the surviving sheet |
| `wooden_bowl.png` | 653/1024 | already a cut-out |
| `rusted_iron_ladle.png` | 208/1024 | already a cut-out |
| `clay_jug.png` | 900/1024 | un-keyed background; needs regeneration |
| `coil_of_rope.png` | 900/1024 | un-keyed background; needs regeneration |

Regenerating those costs one image request each (~$0.039, 1,290 image tokens); that is the
owner's call, not something to spend silently.

### `--draw-probe`: the pixel assertion

None of this was visible to any check in the repo, and a previous session recorded
`Object overlay: loaded 1 created object(s)` as evidence for something that was still broken.
So the harness is a pixel read back from a real frame. `--draw-probe <templateKey>`:

1. switches to a deterministic free camera at zoom 1, and **draws until two consecutive
   framebuffer captures are identical** — the first frames after `Init` are not the settled
   world, and a fixed warmup count passed on one run and failed on the next;
2. finds a visible, unoccupied cell whose **tile draws a sprite**, and spawns the named object
   on it through the real `ObjectFactory`;
3. compares the frame without the object against the frame with it, at every sprite pixel whose
   alpha is 0: those must be **identical** (the tile shows through). A plate behind the sprite
   changes every one of them, and the failure names the first pixel and both colours;
4. also rejects a sprite whose only transparency is its outer rim — that is an un-keyed cell's
   padding, not a cut-out, and it is how `clay_jug` would otherwise pass;
5. removes the `Sprite` and then the `AsciiArt` to check both fallbacks still draw their plate
   and legible ink over it, and paints a catalog tile with no art to check the tile-glyph path;
6. compares the tile's own pixels against its sprite file, so "a tile with art still draws it"
   is asserted rather than assumed;
7. writes the frames to `/tmp/airogue_draw_probe_*.png` on failure. Two lessons are baked into
   that: `rlDrawRenderBatchActive()` is required before the read-back or the capture is a black
   frame (the same line `ImageEncode.cpp` needs, and the same reason), and the *first* version of
   this probe compared five black frames and every differential passed vacuously.

### Verified

`nix-shell -p python3Packages.pillow imagemagick --run 'python3 tools/gen_tile_art.py --cut-check'`
is green, including the new samples.

`./build/AIRogue --draw-probe wicker_basket` under `Xvfb :99`, three consecutive runs, all green:

```
draw probe:  framebuffer 1280x800, camera target (0,0) offset (0,0) zoom 1, mode free
draw probe:  ground cell (5,1) at screen (160,32), tile sprite packs/art/village/sprites/tile_floor.png
draw probe:  tile art: 1024/1024 opaque sprite pixel(s) match the rendered cell
draw probe:  object 'wicker_basket' sprite 32x32 drawn at (160,32) 32x32
draw probe:  transparency: 480 transparent sprite pixel(s) (160 inside the rim), 0 of them changed; 544 opaque pixel(s) changed
draw probe:  no-sprite fallback: 600/1024 plate pixel(s), 424 ink
draw probe:  glyph fallback: 608/1024 plate pixel(s), 416 ink
draw probe:  tile glyph ('sand floor' at 6,1): 736/1024 plate pixel(s), 288 ink
draw probe:  ok (the tile shows through a cut-out object; the glyph and art fallbacks keep a legible plate; a tile with art still draws it)
```

A hand-authored object behaves the same way: `--draw-probe apple_tree` reports 529
transparent pixels with 0 changed, and both fallbacks as `no plate (transparent background), N
pixel(s) drawn over the tile` — because the hand-authored convention is a transparent
`backgroundColor`, where there is no plate to look for and what must hold is only that the art
still drew. That distinction came from running it: the first version counted every pixel as "ink"
for such a template, which said nothing.

The negative control matters as much as the green run. Restoring the pre-fix order (plate first,
sprite over it) in `DrawEntityVisual` and rebuilding produced:

```
draw probe:  tile art: 1024/1024 opaque sprite pixel(s) match the rendered cell
draw probe:  transparency: 480 transparent sprite pixel(s) (160 inside the rim), 480 of them changed; 544 opaque pixel(s) changed
draw probe FAILED: at (160,32) the sprite is transparent but the frame changed from (201,202,200,255) to (90,70,50,255) -- something is painted behind the sprite
```

Every transparent pixel turned into the object's own `backgroundColor`, while the tile layer was
untouched — which is exactly the reported bug, and exactly what the probe is for. `--draw-probe
clay_jug` fails as it should, with the sprite-side message (transparent only around the rim), so
the two causes are distinguishable from the output.

The first attempt at that control drew the plate *after* the sprite rather than before it, which
turned the tiles dim as well (`0/1024` sprite pixels matched) — a reminder that a negative
control has to reproduce the old code and not just the old idea.
