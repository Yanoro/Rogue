# Place derivation from seeds

**Status:** design. Not built. Replaces the place rule in `docs/spatial-memory-design.md` §3.3.

**Date:** 2026-10-17.

**Scope:** how a character decides that a piece of the world is *a place* — where it starts, where it
ends, what kind it is, what it is like, and how one place can contain another. Covers object, tile and
structural seeds, the category taxonomy that gives them meaning, qualities as weighted values, and the
demotion of authored `Location` rects to name seeds.

Companions: `docs/spatial-memory-design.md` (the cell record places are derived from),
`docs/agent-attention-design.md` (why a perceived state needs a real component behind it),
`docs/site-selection-design.md` (spacing signals, still unimplemented).

---

## 1. The problem

The first implementation clustered observed cells by terrain name. On a village where every cell is
`floor`, that produced nine places, all with the same attribute, eight of whose bounding boxes
overlapped. A place appeared whenever a batch of fresh cells landed more than three tiles from an
existing box — so **the rule was arrival order**, and nothing ever merged.

Two things were wrong and they were independent:

- **The clustering was incremental and greedy.** Order-dependent, unmergeable, unexplained.
- **The clustering key was terrain**, which says nothing about what a place *is*. Walls in the village
  are objects (`wooden_wall`, 453 of them), so terrain-based clustering could not see the buildings.

Authored `Location` rects were the other distortion: they claimed every cell inside them, so a market
placed in Town Plaza could never become its own place.

A second pass turned "clusters things that are near each other" into a better primitive — objects and
terrain as **seeds** — but treated a category as a yes/no membership. That is still wrong in three
ways this document fixes:

1. **Structural objects have to contribute** (a well-built wall is a quality of where it stands), yet a
   connected wall chain cannot become a place: the village's 453 structural cells form six structures
   and all six touch, so a distance-linked category over them unions into one enormous chain.
2. **Tiles have to contribute** (a gravel road is a place), which means terrain is not a special case
   but the same mechanism as an object seed.
3. **A thing has many categories at once, with different intensities.** A shabby stall is a market, and
   poor, and dirty, and shady — and that vector, not a single category, is what an agent negotiates
   from.

## 2. Decisions

1. **Objects, tiles and structural objects are seeds.** A place is a cluster of seeds on one
   cluster-forming category.
2. **Categories are data, and carry weights.** A template declares `{category: weight}`; the taxonomy
   declares which categories form places, their thresholds, their link radius, and their split rule.
3. **Kinds and qualities are different jobs.** A place has one *kind* (the category it clustered on)
   and a *vector of qualities* aggregated from every seed inside or bordering it.
4. **Structural objects bound the extent and feed the surrounding place's qualities.** They never form
   places themselves.
5. **Clustering is global, per sample.** Order cannot matter; clusters that grow into each other merge.
6. **Containment is geometric.** A cluster whose seeds lie inside another's extent is a child. The
   taxonomy never declares parent/child edges.
7. **Authored rects stop claiming cells.** Their names survive as name seeds (label + anchor).
8. **`Location` is deleted after the naming verb lands**, so agents keep named travel targets in the
   meantime.
9. **A street with nothing in it is not a place** — unless it is named, or its tiles form one.

## 3. Seeds

### 3.1 What a seed carries

```cpp
struct SeedCategory {
  std::string category;
  float weight = 1.0f;  // 0..1; how strongly this seed contributes
};

struct Seed {
  GamePosition pos;
  std::vector<SeedCategory> categories;  // empty = never seeds
  SeedKind kind = SeedKind::Object;      // Object | Tile | Structural
  flecs::entity_t entity = 0;            // object seeds only
};
```

Weights are **resolved at observation time**: template weights plus whatever instance state says (a
stall that has not been cleaned carries its `dirty` weight into the memory record). Memory therefore
stores what was *seen*, and `lastSeenTick` is the only thing that ages it.

### 3.2 Object seeds

An observed object contributes a seed when it is not `Portable`, not `Structural`, and its template
declares categories. A dropped apple is a fact of the place it lies in, never a place; otherwise every
picnic is a place.

`SeenObject` gains the resolved seed data, so derivation stays a pure function over memory:

```cpp
struct SeenObject {
  // ... existing: templateId, name, pos, character, firstSeenTick, lastSeenTick, vanishedTick
  bool portable = false;
  bool structural = false;
  std::vector<SeedCategory> categories;   // template weights + instance state at observation
};
```

### 3.3 Tile seeds

Tiles carry categories the same way objects do, on the tile definition:

```json
// data/tiles/gravel_path.json
"placeCategories": { "road": 1.0 }

// data/tiles/water.json
"placeCategories": { "water": 1.0 }

// data/tiles/tilled_soil.json
"placeCategories": { "field": 0.4 }
```

`MemoryCell` therefore stores the resolved categories of the tile it observed (empty for plain floor,
grass and other common ground — most cells). A lake, a road and a field are then the same mechanism as
an orchard, and there is no whitelist of terrain classes in C++.

Memory cost: a vector in a structure capped at 2048 entries, empty for the overwhelming majority.

### 3.4 Structural seeds

Walls, fences and rock faces keep two jobs, and they are separate:

- **Bounding.** `Structural` cells stop the extent flood fill, unchanged.
- **Contributing.** A structural object's categories are *qualities*, not kinds: `fancy`, `poor`,
  `weathered`, `new`. A well-built wall around a courtyard makes that courtyard fancy; a patched fence
  makes it poor.

They never form places because a wall chain is connected by definition — the village's six wall
structures all touch, so a distance-linked category over them unions into one chain spanning the
village.

### 3.5 What is not a seed

Portable items, characters, and common ground with no categories — floor, grass, path, unless authored
otherwise. Space between places is space.

## 4. Categories

### 4.1 Kinds and qualities

```json
// data/places.json — what a category is
{
  "market":  { "formsPlace": true,  "minSeeds": 3, "radius": 5 },
  "orchard": { "formsPlace": true,  "minSeeds": 1, "radius": 4 },
  "road":    { "formsPlace": true,  "minSeeds": 2, "radius": 2, "split": "junctions" },
  "water":   { "formsPlace": true,  "minSeeds": 4 },
  "field":   { "formsPlace": true,  "minSeeds": 6 },

  "poor":    { "formsPlace": false },
  "fancy":   { "formsPlace": false },
  "dirty":   { "formsPlace": false },
  "shady":   { "formsPlace": false }
}
```

```json
// data/objects/market_stall.json
"placeCategories": { "market": 0.9, "poor": 0.7, "shady": 0.5, "dirty": 0.6 }
```

`formsPlace` is the whole distinction, and it is data. Anything with `formsPlace: false` cannot make a
place no matter how many of them there are; it only contributes to the qualities of the place it is
inside or borders. That is why a wall's condition is a property of the courtyard and not a chain of
"poor places" across the village.

`minSeeds` stops every stall being a marketplace; `radius` is both the link distance and the outer
bound on extent reach; `split` says how a connected component is cut into places.

### 4.2 Roads

A road network is connected, so a link radius alone yields one place for all of it, which says nothing
about where it goes. `"split": "junctions"` cuts a component at:

- **junction cells** — a road cell with three or more road neighbours,
- **endpoints** — a road cell with one road neighbour.

Each maximal stretch between cuts becomes a place. That is how people name roads ("the north road"),
and it needs nothing but the road cells themselves.

### 4.3 Where the values come from

| source | gives | exists today? |
|---|---|---|
| template `placeCategories` | what kind of thing this is | to add |
| instance state (a condition component, `lastCleanedTick`) | what state it is in | no — designed, deferred |

The distinction matters: a static `"dirty": 0.6` on the stall template says every stall is always
dirty. For "hasn't been cleaned recently" to be true, something must own that fact on the instance —
the same argument `agent-attention-design.md` makes about hunger needing a real component before it
can be perceived. **The engine must not invent a quality it cannot observe.** Until instance state
exists, template values are the only source, and they describe kind, not condition.

## 5. Clustering

### 5.1 Global, per sample

Every vision sample, derivation recomputes the partition from memory:

1. Collect seeds: objects (`memory.objects`) and tiles (`memory.cells`).
2. For each `formsPlace` category, union seeds of that category whose Chebyshev distance is within its
   `radius`.
3. Drop components below `minSeeds`.
4. Apply `split` (§4.2) to the surviving components.
5. Compute each component's extent (§5.2).
6. Match components to existing places by overlap (§5.3).
7. Aggregate qualities (§6).

Nothing depends on arrival order, so the same memory produces the same places every pass, and two
clusters that grow into each other merge because nothing keeps them apart. Cost is linear in cells
(≤2048) and objects (≤256) per sample.

### 5.2 Extent

- **Object cluster:** the observed ground reachable from the seeds, no further than the category's
  radius, not crossing a `Structural` cell. Walls stop the flood, so a house interior is bounded by its
  walls; outside, the radius is the only bound, so an orchard grows as far as its trees do. A missing
  wall means the extent leaks until the radius stops it — bounded, and self-correcting as more walls
  are observed.
- **Tile cluster:** the member cells themselves. A lake is bounded by its shore; a road segment by its
  junctions.

`anchor`, `width` and `height` are the bounding box of the member ground, so the box never claims
ground the character did not observe.

### 5.3 Identity

- A component matches the existing place sharing the most member cells with it. It keeps `id`,
  `label`, `firstSeenTick`; members, extent, kind and qualities are replaced.
- Unmatched components create places; unmatched places are dropped.
- **Split:** one place becoming two keeps its id on the larger component; the other is new and
  unlabelled, so a label never jumps to the smaller half.
- **Merge:** the lower `id` survives with its label; the other's label, if any, becomes a `MemoryNote`
  on the survivor.

## 6. Qualities

For each surviving place:

1. Every seed whose cell lies inside the place's extent contributes its `formsPlace: false` weights.
2. Every structural seed **bordering** the extent (Chebyshev distance 1 from a member cell) contributes
   too — a wall is on the boundary of what it describes, so requiring it to be inside would mean walls
   describe nothing. A wall between two places contributes to both: it is a fact about both sides.
3. Each category's quality is the weighted average, `Σw / n`, over contributing seeds.

```cpp
struct PlaceQuality {
  std::string category;
  float value = 0.0f;  // 0..1
};
```

A stall cluster therefore reads as `kind: market`, `qualities: poor 0.7, dirty 0.6, shady 0.5` — which
is the input an agent needs at the stall, and a different negotiation from "this is a market".

### 6.1 Staleness

Qualities are as fresh as the observations behind them. A place whose seeds were last seen an hour ago
reports the qualities it had an hour ago; the ages are in the seeds and the renderer is expected to say
so. No decay model is built.

## 7. Containment

A component whose seeds all lie inside another component's extent becomes that component's child, one
level deep. No taxonomy edge is involved: the geometry decides, so a sub-place exists for any
containment the world happens to produce, not only the ones an author anticipated.

Depth is capped at one because the place budget is small and a deep hierarchy is unreadable before it
is useful. `KnownPlace::parentId` holds the relation.

The cost is occasional odd children — a chest inside a market is a child `container` place if its
category forms places. That is a taxonomy decision (`minSeeds`, `formsPlace`), not a structural one.

## 8. Name seeds

A name seed is a label and an anchor, owning no geometry:

```cpp
struct PlaceNameSeed {
  std::string label;
  GamePosition anchor;
  KnowledgeSource source = KnowledgeSource::Authored;  // or Told
  bool createsPlace = false;  // authored only: may make a place where nothing else is
};
```

Attach, each pass: a place whose member ground contains the anchor and has no label adopts it; the
closest anchor wins if several match; an unmatched seed stays unused and is never an error.

`createsPlace` is the one authored lever left: a public square is a place because it is *named*, not
because anything stands in it. Told-by uses the same record with a fuzzy anchor.

## 9. The record

```cpp
struct KnownPlace {
  int id = 0;
  int parentId = -1;
  SeedKind seedKind = SeedKind::Object;  // Object | Tile
  std::string kind;                      // the formsPlace category it clustered on
  std::vector<PlaceQuality> qualities;   // weighted averages, sorted by category
  unsigned char confidence = 255;
  GamePosition anchor;
  int width = 0, height = 0;
  std::vector<PlaceFact> facts;          // raw vocabulary counts, unchanged
  std::string label;                     // EMPTY until a name seed or the agent writes one
  int firstSeenTick = 0, lastSeenTick = 0;
  std::vector<std::uint64_t> memberCells;
  std::vector<flecs::entity_t> memberSeeds;
};
```

`attribute` becomes `kind`; `origin` collapses to `seedKind` plus the existing `Origin` anchor place;
`authoringId` disappears with `Location`.

## 10. Authored locations: demotion and deletion

**Now:** `Location` stays in the tree and still answers `[LOCATIONS]` and `[MOVE_TO]`, but derivation
ignores it. Its names are read at load into name seeds.

**Next:** a naming verb (`[NAME_PLACE <id|here> <label>]`) and `[MOVE_TO]` resolving places.

**Then:** delete `Location` — class, editor rect tool, the `locations` array in map JSON,
`[LOCATIONS]`, the location branch of `[MOVE_TO]`, `%LOCATIONS%`, the `[SURROUNDINGS]` "You are at"
branch, the TaskVerification lookup. About 124 references across 12 files.

**Known gap meanwhile:** `%LOCATIONS%` still hands every agent the map's names at spawn, which is the
omniscience `spatial-memory-design.md` §1 set out to remove. It stays until deletion, and fixing it
sooner would mean re-rendering the system turn — a prompt-contract change.

## 11. Debug surfaces

- Overlay label: `#id kind` for an unnamed place, plus its strongest quality (`#3 market, poor 0.7`).
- Swatch list: kind, seed kind, extent, qualities, parent, seed count — so "why is this here" is
  answerable without the JSON.
- JSON dump emits `id`, `kind`, `seedKind`, `parentId`, `qualities` and `memberSeeds`. It currently
  emits none of them, which is why the last round of debugging could not match `#6`/`#7` to the file.

## 12. Stages and acceptance

| stage | what lands | acceptance |
|---|---|---|
| **3a. Object seeds** | `placeCategories` on templates, `data/places.json`, weights, global clustering, extents, stable ids, kind/quality fields, dump ids | 200 floor cells produce no place; one tree makes an `orchard`; three trees within 4 make one `orchard`; two passes over the same memory produce identical places and ids |
| **3b. Authored demotion** | rects stop claiming; names become seeds; seeds attach | Town Plaza's name attaches to a place discovered over its anchor; a market of 3 stalls inside it forms rather than being swallowed |
| **3c. Containment** | `parentId`, geometric, depth 1 | A market with 3 vendor stalls yields 1 parent + N children; no taxonomy edge is consulted |
| **3d. Tile seeds** | tile `placeCategories`, terrain clusters, road junction split | A pond is one `water` place with no objects; a T-shaped road yields three road places, not one |
| **3e. Structural qualities** | structural seeds feed bordering places | A `fancy` wall makes the courtyard fancy; the village does not acquire a giant wall place |
| **3f. Naming verb** | `[NAME_PLACE]`, `[MOVE_TO]` resolves places, `Location` deleted | An agent names a cluster and walks to it by name |

## 13. Deliberately not built

- **Persistence.** Names die with the session; name seeds are what survives.
- **Instance-state qualities.** Designed in §4.3, not implemented: template values only until a real
  condition component exists.
- **Quality decay.** Ages are reported, not modelled.
- **Deeper containment.** One level.
- **Auto-labelling.** The engine never writes `label`.
- **`BlocksVision` as a boundary.** Only `Structural` stops a flood fill.

## 14. Open questions

1. **Place budget.** 24 including children is thin for a market with vendors. Raise it, or count
   children separately?
2. **Double-counted walls.** A wall between two places feeds both. If that over-shares qualities in
   practice, the rule becomes "nearest place" or "the side with more ground".
3. **Road splitting edge cases.** A loop with no junction or endpoint has no cut — one place, or split
   at its longest straight? Diagonal roads make "neighbour" 4- or 8-connected matter.
4. **Kind changes.** A stall cluster that becomes a market keeps its id and label. Right when the label
   says "a stall"?
5. **Quality scale.** Weighted average is the simplest aggregation. A market of one grand stall and ten
   shabby ones should probably read shabby; an average does that only if the shabby seeds outnumber the
   grand one, which they do, but a single dominant object can still pull a small cluster.
6. **Where do `places.json` and template categories live once data packs exist?** Inside the data pack
   (`docs/asset-generation-design.md` §9.3). Until then, `data/`.

## 15. References

- `docs/spatial-memory-design.md` — cells, shapes, caps, the place rule being replaced
- `include/SpatialMemory.hpp` — `ObserveObjects`, `DerivePlaces`, `ToShapes`, `DumpJson`
- `include/Components.h` — `SeenObject`, `KnownPlace`, `Structural`, `Portable`, `Obstacle`
- `src/GameECS.cpp` — observation call site, `HarvestResolutionSystem` (vanishing objects)
- `data/objects/*.json`, `data/tiles/*.json` — the templates that will carry `placeCategories`
- `docs/site-selection-design.md` — spacing signals, currently unimplemented (line 520)
