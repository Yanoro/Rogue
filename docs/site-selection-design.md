# Site selection, the structure record, and construction

**Status: design note. Nothing described here is implemented.**
**Date: 2026-10-16.**
**Scope:** where a blueprint goes, whether it may go there, what is remembered about it afterwards, and
how it comes to exist in the world. The design half — the grammar, the registry, the designer window —
is live; see `blueprint-design.md`.

---

## 1. The gap this closes

`blueprint-design.md` §9 lists four things as not built. Three of them are the same missing pipeline:

| not built | consequence today |
|---|---|
| **Construction** | Nothing materialises a design over turns or consumes materials. `PlaceBlueprintAt` paints it directly, for free, from a debug window. |
| **Site search** | Nothing finds a valid site. Placement is manual and anchor-at-the-mouse. |
| **Ownership** | `owner` is parsed and round-tripped, enforced by nothing. |
| Topology checks | `role` is parsed and inert; a leaking wall or missing door is not noticed. |

The consequence for agents is that the only thing an agent can currently do with a finished design is
look at it. There is no way to ask "where could this go?", no way to be told "not there, and here is
why", and no way to build it.

This document specifies the whole pipeline. The organizing claim is in §2.

---

## 2. The organizing claim: placement is a ranking problem, not a generation problem

An agent cannot be asked to invent a good site. It cannot see terrain (§1 of the gap above is not the
only reason: terrain is invisible to `[SURROUNDINGS]`, see §11), it cannot compute whether a footprint
fits, and a coordinate it invents is a coordinate the engine must second-guess. Aesthetics invented by
a model is the same failure with better vocabulary.

But the engine *can* enumerate every legal site, and it can score them. That turns placement into:

```
engine: enumerate every legal anchor  ->  score each  ->  return the top K with reasons
agent:  choose one, or say what it is looking for
engine: validate the chosen one with the same validator that offered it, then build
```

Every aesthetic, legal and economic constraint becomes a term in a scoring function or a hard filter,
both of which are deterministic, testable in isolation, and explainable in a reply. The model
contributes *intent* and *choice*; the engine contributes *facts* and *legality*. This is constraint 6
of `airogue-direction` ("the engine owns facts, agents own intentions") applied to space.

Two properties fall out and are worth stating as invariants:

- **One validator, two callers.** The gate that offers a site and the gate that builds it are the same
  function. This is the rule `placement-design.md` establishes for `[PLACE]`; it applies identically
  here, or the advertisement and the execution drift.
- **Valid in, valid out.** Only sites that already passed validation are ever shown to a model, so no
  model can pick an illegal site — not by mistake, and not by preference (§7).

---

## 3. What is already there to build on

| piece | where | what it gives us |
|---|---|---|
| `BlueprintExpansion::Expand` | `BlueprintExpansion.hpp` | cells, in part order, as `{x, y, tile, object, carve}` |
| `BlueprintExpansion::Interior` | same | the 4-connected flood fill a sealing check needs; already written |
| `Blueprint::{minX,minY,maxX,maxY}` | `Blueprint.hpp` | the origin-relative bounding box |
| `Game::BuildBlueprintPlacement` | `Game.cpp` | per-cell legality, used by the ghost. Cheap on purpose — the comment says so. |
| `Game::PlaceBlueprintAt` | `Game.cpp` | the stamp: paint tiles, spawn objects, carve |
| `Game::PaintTile` | `Game.cpp` | one implementation of "put this tile here", shared with the map editor |
| `PlaceAtAction`'s validator | `AgentActions.h` | the shape of a rejection message that names ground and reason |
| `Map::GetTile`, `IsInBounds`, `GetLocation`, `GetTilePositionsInRect` | `Map.h` | map access and location lookup |

`BuildBlueprintPlacement` is deliberately *not* the validator this feature needs. It rejects a floor
painted on grass for no reason and does not reject a wall built across a path, because a debug ghost
must be able to show a broken design. §4 replaces it for the agent and player paths while leaving the
debug behaviour alone.

---

## 4. Validity: the layer that runs first

### 4.1 Two kinds of validity, and the mistake of conflating them

**Design validity** is about the blueprint alone: does it parse, does it expand, does it name
materials that exist, does it satisfy the spatial requirements it declares. It needs no map.

**Site validity** is about the blueprint *at a place*: does it fit, is the ground compatible, is
anything in the way, is the ground owned by someone else. It needs a map and an anchor.

`blueprint-design.md` §6.4 and §9 describe "every topological check" as missing and imply it is one
undifferentiated obligation. **That framing is a mistake and this document corrects it.** Not every
blueprint is a building. A camp, a market stall, a shrine, a fence line, a field, a watchtower, a
bridge have no walls to seal and no interior to reach. A universal sealing or entrance requirement
would reject them, which makes the engine enforce a genre assumption that the direction document's
constraint 1 explicitly forbids.

**The rule this document adopts: topology is declared by the design, never assumed by the validator.**
A blueprint states the spatial properties it claims; the validator checks exactly those and nothing
more. A blueprint that declares nothing gets no topology check and is placed as a collection of cells,
which is what a camp is. See §10 for the schema and the correction owed to `blueprint-design.md`.

### 4.2 Site checks (universal, because they are consequences of writing cells)

| # | check | reject reason shape |
|---|---|---|
| 1 | Every cell in bounds | `cell (14,3) is outside the map` |
| 2 | Must-be-free cells free of objects | `blocked by Chest at (14,4)` |
| 3 | Must-be-free cells not on blocking ground | `blocked: wooden wall at (12,7)` |
| 4 | Surface cells on ground the design may overwrite | `cannot lay wooden floor on water at (11,2)` |
| 5 | Passable-after cells currently passable | `the doorway at (9,5) opens onto a wall` |
| 6 | No cell inside a structure owned by another agent | `inside Carl's House, owned by Carl` |

Check 4 needs a rule for what may be overwritten. The precedent is `Placeable::surface`
(`wheat_seed.json` → `["tilled soil"]`), which is a named allow-list on the item. The blueprint
equivalent belongs on the blueprint, as an optional top-level `buries` allow-list of tile names; when
absent, any non-blocking ground may be built over. Keeping it data means "a genre pack decides what
floor may go on what earth" without a line of C++.

Check 6 depends entirely on §5; without the structure record it cannot be written, which is why §5
comes before the rest.

### 4.3 Design checks (only what the design declares)

Declared topology (§10) is checked here, at the anchor, because a leak can be a property of the site as
much as the design — a doorway is only an entrance if the cell outside it is walkable.

Nothing else about the design is re-checked: the registry already refused it at parse time, and
re-checking would give the agent two different answers to the same question.

### 4.4 The reply is the teaching surface

Every rejection names the cell, the obstacle and the rule. This is the same standard
`PlaceAtAction`'s rejection text already sets, and it matters more here: the agent is choosing between
scored options, so a rejection is the only feedback that tells it *why* an option disappeared. A
summary "site blocked" would collapse six distinguishable failures into one and make the transcript
useless for judging the agent's reasoning.

---

## 5. The structure record

### 5.1 The problem

`PlaceBlueprintAt` paints tiles and spawns objects stamped `MapAuthored` with a template key. Nothing
records that a cluster of cells is **one structure**, what it is, or whose it is. So:

- "inside someone else's house" is undetectable — enclosure can be computed, ownership cannot
- "not too close to that building" is undetectable — the building has no edges, only objects
- "align with the neighbouring wall" is undetectable — you cannot reliably infer a wall line from tile names alone
- nothing prevents re-building on top of an existing building's interior

One component closes all four.

### 5.2 The component

A `Structure` marker is stamped on the tile entity of every cell a design occupies — the same cells
`PaintTile` already touches, and the same per-cell shape `MapAuthored` already uses.

```cpp
struct Structure {
  // The blueprint this cell belongs to. Empty for a structure with no blueprint
  // (nothing stamps one today; the field exists so a hand-painted building can be
  // marked without inventing a design).
  std::string blueprintId;
  // Who built it. Empty means unowned, which every ownership check treats as
  // "nobody objects". This is what makes Blueprint::owner stop being inert.
  std::string owner;
  // The world-space bounding box of the whole structure, stamped on every cell.
  // Redundant per cell on purpose: it turns "what building is this cell part of"
  // and "where are that building's edges" into one integer lookup, which is what
  // the scoring signals in §6 need on every candidate anchor.
  int minX = 0, minY = 0, maxX = 0, maxY = 0;

  bool Contains(GamePosition p) const {
    return p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY;
  }
};
```

It is registered like the other data-driven components (`RegisterComponent<Structure>`), so a genre
pack can toggle it and a map file can name it.

### 5.3 Why per cell, not one anchor

An anchor entity would store the footprint once, but every query would become a rect search over all
structures. Per cell costs one small component per occupied tile — a village of twenty buildings of
100 cells is 2,000 components on tile entities that already exist for rendering — and buys `O(1)`
answers to "is this cell inside a building", "whose", and "where is that building's box". The
redundancy is the point.

A feature that genuinely wants the anchor shape (enumerating structures, naming them in a window) can
be built on top later by grouping cells by `(blueprintId, owner, box)`; nothing in this design blocks
it.

### 5.4 What the record is not

It is **not** a location. `Location` stays the authored, map-file concept it is today, with its own
name and description. A structure is something that was built at runtime; a location is something the
map declares. They will often coincide (`Location::GetLocation` returns a house's rect) and they must
not be merged: a location survives a rebuild, a structure is the rebuild.

It is **not** an ownership enforcement mechanism by itself. It records a fact. The rules that read the
fact are policy: check 6 of §4.2, and the deferred per-owner restrictions in §12.

---

## 6. Scoring: the layer that produces aesthetics

Validity is a filter, so aesthetics has to be a score over the survivors. Three signals ship in v1.
They were chosen because each maps to something a person would name as "why that looks wrong", and
because each is computable in `O(footprint)` from data the engine already has.

| signal | symbol | computed as | the failure it prevents |
|---|---|---|---|
| **Terrain uniformity** | `u` | fraction of footprint cells sharing the most common tile name under them | a house straddling grass, path and water, which reads as accidental |
| **Edge alignment** | `a` | count of footprint edges collinear with a neighbouring structure's corresponding edge within the search radius, normalised over the edges that have a neighbour | scattered buildings with no street line — the difference between a village and debris |
| **Entrance frontage** | `f` | 1 when the declared entrance(s) face walkable ground (path preferred), decaying with distance to the nearest path | a front door opening into a wall, a lake, or a building |

The weighted sum lives in one function with the weights in `Defaults.h`:

```cpp
struct SiteSignals { float uniformity, alignment, frontage; };
struct SiteScore { float total; SiteSignals signals; };

float ScoreSite(const SiteSignals &signals);          // weights from Defaults.h
std::string DescribeSignal(SiteSignals);              // one clause per dominant term
```

`DescribeSignal` is not decoration. The reply's reason strings are how an agent learns what this
village values — "aligned with Miller's House wall" teaches the street line without a sentence of
prompt — and they are what makes a placement decision legible in a transcript. A score with no
explanation is unauditable, and unauditable placement is the thing this whole document exists to
avoid.

**What is deliberately not a signal yet:** adjacency to other structures, a duplication penalty for
placing an identical design beside itself, and soft distance-from-home. All three were considered and
left out of v1 — see §12, where each is listed with the reason and the shape it would take. Note the
consequence for `owner`: distance-to-home being absent means the guide's "not too far from your house"
rule is still the agent's judgement rather than a scored term, which is a defensible first position
and an explicit deferral rather than an oversight.

---

## 7. The preference layer: subjective language, engine evaluation

### 7.1 The need

"There is no space" and "there is space, but not a place I want to be" are different answers. A farmer
expanding a field wants flat open ground; an introverted agent building a house wants to not be on the
road; a merchant wants frontage on the plaza. The engine can score the first kind of preference from
geometry. It cannot score the second, because "quiet" is not a geometric property.

This is where a small model contributes something the engine cannot compute, and a **System 1 model is
genuinely well suited to it**: a classifier of this kind selects rather than generates, so it is cheap
and fast, and it **cannot invent a coordinate**. It is also at its best on a question decomposed into
its simplest elements, which a text list of valid candidates already is.

Nothing in this section is tied to a particular implementation. "System 1 model" means whatever
satisfies the `SystemOneClassifier` interface in `agent-attention-design.md` §4.2 — a hosted decision
model, a local ONNX classifier, or a rules fallback. Which one is in use is a setting (§7.5), and the
design does not change if the answer changes.

### 7.2 The form, and the flaw it avoids

The naive form is "give a vision-capable model an image of the area and the requirements, and let it
judge". That form is rejected: any image is rendered from engine state, so the engine already knows its
contents; rendering to pixels to recover them through a vision model is a lossy round trip; and a
screenshot depends on camera position and zoom, so the same world would produce different verdicts and
an agent could present a flattering crop. Vision on generated terrain is deferred for those reasons,
not because it is uninteresting.

The form adopted instead keeps the interpreter **inside the weight vector**:

```
agent:  [SITE small_house NEAR my farm WITH PREFERENCE "a quiet place without much foot traffic"]
engine: resolve anchor, enumerate, validate, score -> top K valid sites
interp: the free-text preference -> weights over the known signals  (System 1, or the main model)
engine: rescore the top K with those weights, clamp, re-rank, reply
```

The interpreter's output is a weight vector, never a site. The engine always does the arithmetic, so:

- the preference can never rescore anything that failed validation (valid in, valid out)
- the result is explainable in the same reason strings as the default score
- a bad weight vector produces a suboptimal ranking, not an illegal build
- the interpreter never sees the world, only the shortlist — bounded input, cheap call

The weights are clamped per signal (`±kMaxPreferenceWeight`, `Defaults.h`) so one adjective cannot
erase terrain uniformity entirely, and the interpreter returns a rationale string that is echoed in the
reply, so the translation is visible: *"quiet: penalising foot traffic and frontage"*.

### 7.3 A second, better use of the same call

There is a variant worth preferring where the engine's signals already cover the request: have the
interpreter decide **which signals the phrase refers to**, not the weights. "A quiet place" is
`low foot traffic`, full stop; the magnitude is the engine's business. The interpreter then becomes a
vocabulary bridge from the agent's words to the signal list, which is a small, verifiable, testable
mapping rather than a numeric judgement, and the reply can say so.

Both variants are the same call to the same client; the difference is how much of the answer is
asserted by the model. Start with the weight form, and move toward the signal-selection form if the
weights turn out to be erratic — which they may, because a model's sense of "0.8 of the way toward
quiet" is not calibrated to anything.

### 7.4 What the interpreter must be told

The signal vocabulary, by name and in one clause each: `foot_traffic` (how much of the surrounding
area is occupied or recently walked), `open_space` (how enclosed the site is), `frontage` (whether it
faces a path), `alignment`, `uniformity`. The vocabulary is *engine-authored and stable* — it is
surfaced in the `[SITE]` reply so an agent can phrase a preference the interpreter can actually map,
and so a preference that cannot be mapped gets an honest "I do not know that signal" rather than a
silent zero.

`foot_traffic` is the one signal with no existing source; §12 records the cheap proxy (occupancy and
recent-mover density in the radius) and the honest alternative (a real pathing-frequency counter,
which the `PlayerPath`/pathfinding systems could supply later). What must not happen is a signal the
interpreter can name and the engine cannot compute — that is a silent lie in the ranking.

### 7.5 Determinism, and the price of this layer

With no preference, `[SITE]` is fully deterministic: same world, same top K, same order. With a
preference it is not, because the interpreter may be a sampled model. Three consequences:

- The preference must be **recorded with the shortlist**. An agent may `[SITE]` in one turn and
  `[BUILD]` in the next; the build re-validates the offered anchor, and if the world changed in
  between it must be able to say "the site I offered you is now blocked" rather than silently building
  somewhere else.
- The interpreter is a **setting**, defaulting to off (the deterministic scorer alone), so the feature
  degrades to auditable behaviour with one flip.
- Which interpreter was used, and what it returned, is **logged** like any other model call, so a
  surprising ranking can be traced. This is the same decision the guide made about answers being
  permanent in `ctx.history`: a model's contribution to game state has to be inspectable.

---

## 8. Search: enumerating the candidates

```cpp
struct SiteQuery {
  std::string blueprintId;
  GamePosition anchor;     // what "NEAR" resolved to
  int radius = kDefaultSiteSearchRadius;   // in tiles, around the anchor
  int maxResults = kDefaultSiteResults;    // how many ranked sites to return
  std::string preference;  // may be empty; §7
};

struct SiteVerdict {
  GamePosition origin;
  int orientation = 0;              // 0..3; see below
  float score = 0.0f;
  std::vector<std::string> reasons; // §4.4 and §6
};

std::vector<SiteVerdict> FindSites(const SiteQuery &query);
SiteVerdict EvaluateSite(const Blueprint &, GamePosition origin, int orientation);
```

The loop is: for each candidate anchor in the radius, for each orientation, run §4's checks, compute
§6's signals on success, keep the best `maxResults` by score. Candidate anchors should be pre-filtered
so the expensive path runs rarely: an anchor whose bounding box is out of bounds or whose
must-be-free cells are occupied is rejected before any signal is computed. The occupancy map is built
once per query, not once per candidate — the same discipline `BuildBlueprintPlacement` already uses for
the ghost.

**Rotation ships with this**, because it is nearly free here and it is what makes frontage satisfiable:
rotating a cell set is `(x, y) -> (-y, x)` applied to origin-relative coordinates, four times. No
schema change, no re-authoring, and `ComputeBounds` follows. `blueprint-design.md` §9 lists rotation as
not built; this is where it lands, as an orientation parameter on the site rather than a property of
the design.

**Anchor and radius.** `NEAR` resolves through the existing name lookup — a `Location` name, a
character's `DisplayName`, or an object — and the anchor is that thing's position. Bare coordinates are
accepted for `[BUILD]` (the agent is committing to a specific answer the engine gave it) and refused
for `[SITE]` (the agent is asking a question, and "somewhere near (30,40)" is a question the engine
answers badly and the agent cannot phrase well). This asymmetry is deliberate and mirrors the existing
DSL's stance that bare coordinates are not valid targets for `[MOVE_TO]`.

---

## 9. The verbs

Both are registered before `[GENERIC_INTERACT]` in `Game::ECSInit`, for the reason
`guide-command-design.md` §8 records: that catch-all matches `[A-Z_]+` and would otherwise swallow
them. Both are visible, unlike `[PLACE]`, because placement is not a power-user affordance in a
building game.

```
[SITE $BLUEPRINT NEAR $AREA] [WITH PREFERENCE "..."]
     where could this go? Returns the ranked valid sites with reasons,
     and names the signals a preference may refer to.

[SITE $BLUEPRINT] [WITH PREFERENCE "..."]
     with no area, searches around the agent.

[SITE]
     with no blueprint, lists the blueprints that can be built and what each needs.

[BUILD $BLUEPRINT AT X,Y]
     commit to a site. Runs the same validator; refuses with the same reasons.
```

Example reply — the shape that matters, because this is what the agent reasons over:

```
System: 3 sites for Small House near Pietro's Farm (searching 12 tiles, 4 orientations each):
- (12,4) score 0.81: flat grass, aligns with Miller's House wall line, door faces the path
- (20,7) score 0.58: flat grass, 14 tiles from your farm, no frontage
- (8,10) score 0.41: mixed grass and path under the footprint
Rejected: (30,4) overlaps Carl's House at (32,5); (14,4) blocked by Chest at (14,4); (13,3)
inside Pietro's Farm's tilled field, which is not ground this design may bury.
System: A preference may mention: foot_traffic, open_space, frontage, alignment, uniformity.
```

Note what the reply does *not* do: it does not pick. The choice stays with the agent (§2), because
choosing between valid sites on stated reasons is exactly the reasoning this whole exercise is meant to
exercise, and a single returned site would make the agent's spatial judgement unobservable.

---

## 10. Declared topology, and two corrections owed to `blueprint-design.md`

### 10.1 The schema addition

An optional top-level block on a blueprint. Absent means *no topology is claimed and none is checked*,
which is the correct default for a camp or a stall.

```json
{
  "id": "small_house",
  "topology": { "enclosed": true, "entrance": "required" },
  "parts": [ ... ]
}
```

| key | values | checked as |
|---|---|---|
| `enclosed` | `true` / absent | `Interior()` over the design's own cells must return a non-empty interior, and the declared interior must not be 4-connected to outside the bounding box |
| `entrance` | `"required"` / `"discouraged"` / absent | at least one cell whose declared role is `entrance` (or a `carve` adjacent to the interior) must have walkable ground outside the footprint |

Both are checked at the site (§4.3) because both depend on the ground around the anchor. Both are
reported by name: `declares enclosed: the walls leak at (4,5)`, `declares an entrance: every doorway
opens onto wooden wall`.

`role` stops being decorative here, and stops being universal: it matters only when a design claims
something in terms of it. The inert `role: "structural"` / `"fixture"` values remain inert, because
nothing in this design needs them.

### 10.2 Correction 1 — topology is not a universal obligation

`blueprint-design.md` §6.4 says the missing checks are "whether the walls seal, whether there is an
entrance, whether the interior is reachable", and §9 lists "every topological check" as a gap, without
distinguishing kinds of structure. Read literally, that makes a market stall invalid for having no
interior, and a camp invalid for having no sealing wall. **This is a mistake in that document and
should be corrected to say that topology is declared per design and checked only when claimed.** The
underlying engine machinery (`Interior`) is unchanged and still lives where §3 of that document puts
it.

### 10.3 Correction 2 — `owner` gains a meaning, and it is not enforcement

`blueprint-design.md` §2.1 and §9 both say `owner` is persisted, round-tripped and enforced by
nothing. With §5, a built structure records its owner, so `owner` acquires its first reader: check 6 of
§4.2, which refuses a site overlapping another agent's structure. **The correction is to that
document's wording, not the field.** It should say that `owner` is read by site validation once
construction exists, and that the standing decision — all agents may *see and use* every design —
remains true: an owner restricts where a structure is built, never which blueprint an agent may copy
or place. Those are different questions and the document conflates them by listing ownership as one
item.

---

## 11. Construction

`BuildBlueprintAt(blueprint, origin, orientation, builder)`:

1. Re-run `EvaluateSite`. Refuse with the same reasons if it fails — including the case where the
   world changed since the shortlist was offered (§7.5).
2. Stamp every cell exactly as `PlaceBlueprintAt` does today: `carve` clears the cell's object and
   leaves the ground, `place` spawns a `MapAuthored` object, `paint` calls `PaintTile`. One constructor,
   called by the debug window, the player path and the agent verb — this is decision 1 of the answers
   below.
3. Stamp the `Structure` marker on every occupied cell with the blueprint id, the builder's name, and
   the footprint box (§5.2).
4. Return a report naming what was built, where, and which cells were painted/placed/carved.

**Instant, for v1.** No timer, no materials. Construction that consumes time and goods is the
simulation-honest end state, and the debug window's free direct paint must not become the agent's path
by accident — but making construction expensive before placement is *correct* would mean debugging a
placement bug through a materials shortage, and would couple this feature to `AgentAction` interruption
handling. §12 records the two follow-ups (a timer, then a material requirement derived from the
design) as separate steps with their own contracts.

### 11.1 What the guide needs to say

This feature is only usable if the agent knows it exists. The verbs are visible in the spawn prompt's
command list, but the *rules* — that a site must not be inside someone else's building, that
distance from home is the agent's own judgement, that a preference string exists — belong in the
farming/building guide, alongside the location rules already planned there. The guide supplies the
procedure; `[SITE]` supplies the facts; the agent supplies the choice. That division is the same one
`guide-command-design.md` decisions 9, 10 and 12 establish, and this feature does not change it.

---

## 12. Not built, and deliberately

| thing | why not now | the shape it would take |
|---|---|---|
| Construction time | couples placement correctness to action interruption before placement is proven | an `AgentAction` with an `ActionTimer`, like `HarvestAction` |
| Material cost | needs a per-blueprint requirement derivation that does not exist | sum the design's paint/place materials into an `ItemStack` list; a stockpile or inventory check in `EvaluateSite` |
| Multi-level building | `Map` is single-layer; only level 0 is placeable | unchanged from `blueprint-design.md` §9 |
| Ownership as a *construction* restriction | all agents may build anywhere they legally can, for now | read `Structure::owner` in check 6 and refuse a site whose neighbours belong to someone who has not agreed |
| Pathing-frequency `foot_traffic` | needs a counter nothing keeps | count pathfind traversals per tile with decay; the pathfinding systems already compute the routes |
| Adjacency / duplication / distance-from-home signals | three more weights with no evidence yet about what they do to the result | add to `SiteSignals` and `ScoreSite`; each is `O(footprint)` or `O(structures)` |
| Vision-based aesthetic judgement | a screenshot is a lossy, camera-dependent view of state the engine already has | render a real frame, ask whether the *village* reads as coherent, never whether a site is legal |
| Blueprint rotation in the designer/preview | rotation is a placement property here, not a design property | preview pane gains an orientation control reusing the same cell transform |

---

## 13. Open questions

- **Q1. Which is the default ranking, engine or interpreter?** This document makes the engine's score
  canonical and the interpreter an opt-in rescoring. The alternative — the interpreter always runs,
  the score is a tiebreak — would make placement more surprising but more varied. Unresolved, and it
  should be decided on observed transcripts rather than in advance.
- **Q2. How large is the search, and does it need a coarse pass?** Radius 12 with four orientations is
  roughly 2,300 candidates per query; the pre-filter makes that cheap, but a large structure in a dense
  village may want a coarser occupancy pass before per-cell validation. Measure before optimising.
- **Q3. Does `[SITE]` belong in the guide or the prompt?** §11.1 puts the rules in the guide. If agents
  are observed never asking, the answer is a prompt line, and that is a prompt-authoring decision, not
  an engine one.
- **Q4. What happens when a *design* is valid but no site is?** Today the reply is "no valid sites in
  range". The interesting behaviour — "nothing here, the nearest place with room is 30 tiles east of
  the village" — is a second, wider search, and it is where the agent's first real spatial negotiation
  would happen. Deferred, and it is the same question the earlier farming discussion raised about
  fields that do not fit.
- **Q5. Should an offered site expire?** §7.5 assumes the build re-validates and reports a stale site
  as blocked. An alternative is a reservation that expires with a timestamp. Unresolved; the
  re-validation behaviour is strictly simpler and is what v1 assumes.

---

## 14. References

- `docs/blueprint-design.md` — the grammar, the registry, the designer window, §9 "not built"
- `docs/placement-design.md` — the validator pattern this reuses, and the `[PLACE]` split
- `docs/guide-command-design.md` — decisions 9/10/12 (facts, not routes), §8 on registration order
- `docs/tile-art-wiring-design.md` — what a *rendered* area looks like, for the deferred vision work
- `include/Blueprint.hpp` — the data model; `include/BlueprintExpansion.hpp` — cells, `Interior`, bounds
- `include/Components.h` — `MapAuthored`, `Hint`, and the component registration it will sit beside
- `src/Game.cpp` — `BuildBlueprintPlacement`, `PlaceBlueprintAt`, `PaintTile`,
  `BuildDesignerSystemPrompt`
- `src/GameDraw.cpp` — the world-space ghost, which should call the same validator
- `include/Map.h` — `GetTile`, `IsInBounds`, `GetLocation`, `GetTilePositionsInRect`
- `src/AgentActions.h` — `PlaceAtAction` for the rejection-reply shape; `GuideAction` for a read-only
  query verb
- `include/Defaults.h` — where the site-search radius, result count and score weights go
