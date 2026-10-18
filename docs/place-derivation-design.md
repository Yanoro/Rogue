# Place derivation from seeds

**Status:** design. Parts implemented (3a object seeds, 3b name seeds). Replaces the place rule in
`docs/spatial-memory-design.md` §3.3.

**Date:** 2026-10-17.

**Scope:** how a character decides that a piece of the world is *a place* — where it starts, where it
ends, what it is like, and how the same place can be several things at once. Covers object, tile and
structural seeds, the category taxonomy, **aspects as distance-decayed fields**, and the demotion of
authored `Location` rects to name seeds.

Companions: `docs/spatial-memory-design.md` (the cell record places are derived from),
`docs/agent-attention-design.md` (why a perceived state needs a real component behind it),
`docs/site-selection-design.md` (spacing signals, still unimplemented).

---

## 1. The problem

Three findings, each from testing the previous one.

**The first rule was arrival order.** Clusters of observed terrain produced nine places with the same
attribute on a single-surface village, most of whose bounding boxes overlapped, because a place
appeared whenever fresh cells landed far from an existing box and nothing ever merged.

**The second rule treated a category as yes/no.** Objects and terrain became *seeds* and clustered per
category. That works, but a category was either present or absent, so a thing could not be a market and
poor and dirty at once — which is the vector an agent actually negotiates from.

**The third rule made the vector a scalar per place.** Even after categories became weighted, the weight
was averaged over the whole extent. A market next to an orchard then had an orchard aspect *everywhere*,
when the truth is that the orchard is strong at one edge and negligible at the other. The aggregation was
wrong, not the merge: a scalar cannot be true of "the east corner only".

What survives all three: a place is a **region of observed ground with seeds in it**, and its character
is a **field over that region**, not a number about it.

## 2. Decisions

1. **Objects, tiles and structural objects are seeds.** A place is a cluster of forming-category seeds
   over observed ground.
2. **Categories are data and carry weights.** A template declares `{category: weight}`; the taxonomy
   declares which categories form regions, their seed thresholds, their link and influence radii, and
   their split rule.
3. **One region per connected cluster, across categories.** A market next to an orchard is one place
   with two aspect fields, because the merge is not the problem — uniformity was.
4. **Aspects are fields, evaluated at a point.** `aspectAt(place, cell, category)` decays with distance
   from the seeds that contribute it. The place keeps a summary (peak, where it peaks, mean) for display
   and ordering, and the sources for evaluation.
5. **Clustering is global, per sample.** Order cannot matter; clusters that grow into each other merge.
6. **No hierarchy.** Enclosed ground is already a separate region because the extent flood stops at
   walls; adjacent ground is a separate region because it is not linked; a weak signal on continuous
   ground is an aspect. Names are aliases on one region, not child regions.
7. **Authored rects claim nothing.** Their names are name seeds, attaching to whatever region covers the
   anchor.
8. **`Location` is deleted after the naming verb lands.**

## 3. Seeds

### 3.1 What a seed carries

```cpp
struct SeedCategory {
  std::string category;
  float weight = 1.0f;  // 0..1
};

struct Seed {
  GamePosition pos;
  std::vector<SeedCategory> categories;
  SeedKind kind = SeedKind::Object;  // Object | Tile | Structural
  flecs::entity_t entity = 0;
};
```

Weights are resolved at observation time: template values plus whatever instance state says. Memory
records what was seen; `lastSeenTick` is the only thing that ages it.

### 3.2 Object seeds

An observed non-portable, non-structural object whose template declares categories. `SeenObject`
carries the resolved data, so derivation stays a pure function over memory:

```cpp
struct SeenObject {
  // templateId, name, pos, character, firstSeenTick, lastSeenTick, vanishedTick
  bool portable = false;
  bool structural = false;
  std::vector<SeedCategory> categories;
};
```

Everything template-derived is refreshed on **every sighting**, not only when the record is created:
the map editor overwrites whatever stands on a cell through `ApplyTemplate`, and `Evolvable` growth
re-applies a template in place. A record that kept its first sighting's categories was a chest whose
seed data still said "wooden wall" — visible, and never a place.

### 3.3 Tile seeds (stage 3d)

```json
// data/tiles/water.json
"placeCategories": { "water": 1.0 }
```

`MemoryCell` stores the resolved categories of the tile it observed, empty for common ground. A lake,
a road and a field become the same mechanism as an orchard, with no terrain whitelist in C++.

### 3.4 Structural seeds

Walls bound and contribute, as two separate jobs:

- **Bounding.** `Structural` cells stop the extent flood.
- **Contributing.** A wall's categories (`fancy`, `poor`, `old`) feed the aspect field of the place it
  borders, and are counted in that place's facts as what it is made of.

The contribution rule, settled:

- **Contact is orthogonal.** A wall contributes to a place with a member cell on one of its four sides.
  A corner touch is not an enclosure, and a diagonal wall must not smear its quality into a room it
  does not bound.
- **Both sides inherit.** A wall is the material of the boundary, not a property of one room; a stone
  wall between a chapel and a yard makes both of them stony.
- **It decays like any other source**, so a wall reads strongest against itself and weaker across a
  hall. Sources are subsampled per category the same way tile cells are.
- **A named area has walls too.** A place that exists only because it is named still inherits the
  structure it touches; `primaryAspect` stays `named` because no category formed it.

They never form regions: the village's 453 structural cells form six structures and all six touch, so a
distance-linked category over them is one chain spanning the village.

### 3.5 What is not a seed

Portable items, characters, and common ground with no categories. Space between places is space.

## 4. Categories

```json
// data/places.json
{
  "market":  { "formsPlace": true,  "minSeeds": 3, "radius": 5 },
  "orchard": { "formsPlace": true,  "minSeeds": 1, "radius": 4 },
  "road":    { "formsPlace": true,  "minSeeds": 2, "radius": 2, "split": "junctions", "linksAcross": false },
  "water":   { "formsPlace": true,  "minSeeds": 4, "linksAcross": false },
  "poor":    { "formsPlace": false, "radius": 4 },
  "dirty":   { "formsPlace": false, "radius": 3 }
}
```

```json
// data/objects/market_stall.json
"placeCategories": { "market": 0.9, "poor": 0.7, "dirty": 0.6 }
```

- **`formsPlace`** — whether a category can *form* a region. Both kinds become aspects; the flag only
  decides whether the seeds can start a place of their own.
- **`minSeeds`** — how many seeds of that category a cluster needs before the category counts as an
  organising reason rather than a quality. One stall is not a market; three are.
- **`radius`** — two jobs, both distance-based: the extent bound for clustering, and the **influence
  radius** of the aspect field (the distance at which a seed's weight decays to zero).
- **`split`** — how a connected region is cut (roads).
- **`linksAcross`** — whether seeds of this category may join seeds of *other* categories. False for
  linear terrain: a road or a lake is its own region. Without it, one path across the village chains
  every object within the link radius into a single place.

The distinction that matters for behaviour is not kind versus quality but **where the value is
evaluated**: inside the region, near a stall, the market aspect is high; at the far corner it is low.

### 4.1 Roads

A road is a chain, so one component would be "all the roads" and say nothing about where they go.
`"split": "junctions"` cuts a road component into the stretches between **junctions** (a cell with three
or more road neighbours) and **endpoints** (one). Each stretch is a place, and a cut cell belongs to
exactly one stretch — the one holding its lowest `(y, x)` neighbour — so no cell is claimed twice or
orphaned. A loop with no junction has no cut and stays one place.

A cell connects to its **orthogonal** road neighbours; diagonals count only for a cell with no
orthogonal neighbour. With plain 8-adjacency the cell just below a T junction is diagonally adjacent to
both arms and reads as a four-way crossing, which cut a one-cell stretch out of the junction's shadow.
Orthogonal-first keeps a T as three stretches and still connects a road drawn diagonally.

## 5. Clustering

### 5.1 Global, per sample

Every vision sample, derivation recomputes the partition from memory:

1. Collect seeds: objects (`memory.objects`) and tiles (`memory.cells`, 3d).
2. Union seeds within `DEFAULT_PLACE_LINK_RADIUS` (Chebyshev), **regardless of category**: a market
   next to an orchard is one region whose aspect fields overlap.
3. A component becomes a place if at least one of its forming categories meets its `minSeeds`.
4. Extent: observed ground reachable from the seeds, bounded by the largest `radius` among the
   component's forming categories, not crossing `Structural`.
5. `split` cuts the region where a category asks for it (roads, 3d).
6. Match components to existing places by member-cell overlap; aggregate aspects.

Nothing depends on arrival order. A component that grows into another merges, because nothing keeps
them apart.

**The one thing to watch:** linking across categories makes object-dense ground chain. `linkRadius` is
the knob; if a village square of stalls and trees turns into one place, that is this radius, not a bug.

### 5.2 Identity

- A component matches the existing place sharing the most member cells. It keeps `id`, `labels`,
  `firstSeenTick`; members, extent, aspects are replaced.
- Unmatched components create places; unmatched places are dropped.
- **Split:** one place becoming two keeps its id on the larger component; the other is new and
  unnamed, so a label never jumps to the smaller half.
- **Merge:** the lower `id` survives; labels from both are kept, which is why labels are a list.

## 6. Aspects

### 6.1 The field

```cpp
struct AspectSource {
  std::string category;
  GamePosition pos;
  float weight;   // 0..1, resolved at observation
  float radius;   // influence radius from the taxonomy
};

float aspectAt(const KnownPlace &place, GamePosition cell,
               const std::string &category);
```

For a cell and a category, the value is the **maximum** over that category's sources of
`weight × decay(distance(cell, source.pos))`, with linear decay to zero at the source's radius:

```
decay(d, r) = max(0, 1 - d / r)      // Euclidean d
```

Maximum rather than sum: two shabby stalls beside each other do not make a place twice as poor, and a
sum would make a dense market arbitrarily exceed 1.

The market-next-to-orchard case then reads:

```
at the market's far corner, 30 tiles from the orchard   orchard 0.0   market 0.9
at the orchard edge, 3 tiles from the trees             orchard 0.6   market 0.3
```

Aspect queries are **point queries**. That is the whole correction: the engine's answer to "what is
this like" is evaluated where the character stands.

### 6.2 The summary

A place still needs one number per category for the window, for ordering, and for any digest:

```cpp
struct PlaceAspect {
  std::string category;
  float peak;            // highest weight among sources, decay 1 at its own cell
  GamePosition peakPos;  // where that peak is
  float mean;            // area-weighted average over the place's member cells
};
```

`primaryAspect` is the forming category with the highest **mean**, used for display and sorting. Mean
rather than peak because the peak picks a single seed: a market with one apple tree in it has
`orchard` peak 1.0 and `market` mean 0.17, and a reader asked "what is this place" means the area, not
the tree. It is a label for humans, not an identity: two places can both be `market`.

### 6.3 Sources

A place's sources are the seeds it owns — its own forming category included, so a market's own stalls
decay too and its edge is less "market" than its middle. Below-threshold forming categories of owned
seeds contribute as sources too, which is what makes a single stall an aspect of the field it stands in
rather than a place of its own.

Structural seeds are excluded until 3e.

### 6.4 Staleness

A field is as fresh as the observations behind it. A place whose sources were last seen an hour ago
reports the field it had an hour ago; ages are in the seeds and the renderer is expected to say so. No
decay over time is modelled.

## 7. Geometry instead of hierarchy

There are no child places. The three cases a hierarchy was there to express are geometry:

| situation | outcome | why |
|---|---|---|
| a walled room inside a building | a separate place | the extent flood stops at `Structural` cells |
| an orchard beside a market | a separate place | not within `linkRadius`, or a separate component |
| one stall in a field | an **aspect**, not a place | `market` does not meet `minSeeds` |

The one thing hierarchy did that geometry cannot is give part of a region its own *name*. That is solved
by letting a place carry several labels: names are aliases for a region, and a second name is added
rather than rejected. "The Library" and "the old reading room" describe the same ground, which is how
people talk about buildings anyway.

## 8. Name seeds

```cpp
struct PlaceNameSeed {
  std::string label;
  GamePosition anchor;
  KnowledgeSource source = KnowledgeSource::Authored;  // or Told
  bool createsPlace = false;
  int radius = 3;   // how far a created named area may spread
};
```

Attach, each pass:

1. If a place's ground covers the anchor, its label is added to that place's `labels` (deduplicated).
2. Otherwise, if the seed may create a place **and the anchor's cell has been observed**, a place of
   kind `named` is created whose extent is the observed ground within `radius`.
3. Otherwise it stays unused. An unused seed is never an error.

**A name is not an observation.** An object seed contributes its own cell unconditionally — seeing the
object is seeing the ground. A name whose anchor was never looked at must not conjure a place.

Authored seeds take `radius` from the `Location`'s larger side (halved, clamped 2–8): the rect is a
size hint, never a claim.

## 9. The record

```cpp
struct KnownPlace {
  int id = 0;
  PlaceOrigin origin = PlaceOrigin::Inferred;
  KnowledgeSource source = KnowledgeSource::Observed;
  unsigned char confidence = 255;
  GamePosition anchor;              // top-left of the observed extent
  int width = 0, height = 0;
  std::vector<PlaceFact> facts;     // raw vocabulary counts
  std::string primaryAspect;        // display only; strongest forming peak
  std::vector<PlaceAspect> aspects; // summaries, sorted by peak descending
  std::vector<AspectSource> aspectSources;  // the field
  std::vector<std::string> labels;  // aliases; empty until named
  SeedKind seedKind = SeedKind::Object;  // Object | Tile | Named
  int firstSeenTick = 0, lastSeenTick = 0;
  std::vector<std::uint64_t> memberCells;
  std::vector<flecs::entity_t> memberSeeds;
};
```

Removed by this design: `parentId`, `kind` (now `primaryAspect`), `qualities` (now `aspects`),
`label` (now `labels`), `authoringId` (gone with `Location`).

## 10. Authored locations: demotion and deletion

**Done (3f).** `Location` is gone: the class, its loader and writer, the editor's rectangle tool, the
`locations` key in map JSON, `[LOCATIONS]`, the location branch of `[MOVE_TO]`, `%LOCATIONS%`, the
map lookup behind the `[SURROUNDINGS]` "You are at" branch, and the TaskVerification lookup.

What replaced each:

| was | is |
|---|---|
| `locations` rectangles in the map | `placeSeeds`: `{"label", "position", "radius"}`, nothing else |
| editor rectangle drag | a one-cell name stamp with a radius spinner |
| `[LOCATIONS]` (the map's names) | `[PLACES]` (what this character remembers, nearest first) |
| `%LOCATIONS%` in the system prompt | nothing; a character must ask, and can be wrong |
| `[MOVE_TO] <location name>` | `[MOVE_TO] <place name>`, resolved against memory |
| `[SURROUNDINGS]` "You are at <location>" | "You are at <place>", from memory |
| Editor inspection of a rectangle | the seed on the cell, and what the player remembers there |

The naming verb is `[NAME_PLACE here <name>]` or `[NAME_PLACE #<id> <name>]`. It **adds** a name
rather than replacing one, because a place can be "the Library" and "the old reading room"; naming
ground no place covers records a seed, and the next derivation pass grows a place from observed ground
around it. The engine still never invents a label.

Old maps are still readable: a `locations` array is read once as seeds (centre point, radius from the
rectangle) when `placeSeeds` is absent, so deleting the class did not silently drop every authored name
from a map written before it.

## 11. Debug surfaces

- **Aspect tint.** Member cells are tinted by their *dominant local aspect* rather than by place
  identity, which is the only way to see a field instead of trusting a number. Colour is a stable
  function of the category id.
- **Outline and label** stay per place: `#id primaryAspect`, or the first label once it has one.
- **Swatch list**: labels, primary aspect, per-aspect peak and where it peaks, seed count.
- **JSON dump**: `labels`, `primaryAspect`, `aspects` (summary), and `aspectSources` in full detail
  mode, because the field is what the summary is a guess about.

### 11.1 The test map

`maps/Place_Test.json` (64×64, grass base, 77 objects) exists to exercise derivation end to end.
Each area is one acceptance case:

| area | what it proves |
|---|---|
| orchard at (8–13, 8–11) + three stalls at (15–17, 9–11) | one region with two fields: `orchard` 1.0 at the trees, 0.5 at the first stall, 0 at the far stall |
| tilled soil (26–35, 4–9), four wheat, two stalls | tiles and objects in one place; two stalls are `market` 0.9 as an **aspect** (threshold 3) |
| pond (6–12, 20–31) with a stream | water region bounded by its own cells, `linksAcross: false` |
| T road at (15, 32) | three stretches, one junction |
| four-way cross at (34, 32) | four stretches |
| road loop (2–8, 40–44) | no junction, so one place |
| diagonal road (40–45, 42–47) | orthogonal-first adjacency keeps it connected |
| three chests at (24–26, 16–17) | one storage place from three seeds |
| iron veins + smeltery + anvil at (34–38, 16–18) | overlapping `mine` and `workshop` fields |
| chapel (20–27, 22–29), door gap at (23, 29) | walls bound the extent; six religious seeds |
| empty walled yard (28–33, 19–23) | a name creates a place over observed ground, bounded by walls |
| one tree at (44, 20), two stalls at (42, 10), one wheat at (44, 28) | `minSeeds` 1 / below 3 / below 4 |
| apples and ore at (20–22, 40) | portable items never seed |
| nine locations, including "The Orchard" and "Market Square" on one region | names are aliases, not identities |

The map's `positions` array is written **y inner, x outer**, which is the order `Map::LoadMap` walks it
(`src/Map.cpp`). Writing it row-major transposes the tile layer against the objects, which are placed by
explicit coordinates — the first version of this map did exactly that, and it looked like a derivation
bug (water appearing where the pond was not).

Expected result: 17 places (7 object places, 9 road stretches, 1 named yard), all under the 24 cap.
`market_stall.json` is real content added for this map; `stone_wall`, `mossy_stone_wall` and
`wooden_wall` now carry `fancy`/`old`/`poor` categories, ready for 3e.

## 12. Stages

| stage | state | what lands |
|---|---|---|
| **3a. Object seeds** | done | per-category clustering, global, stable ids, weights |
| **3b. Name seeds** | done | authored names as seeds, `createsPlace`, unused seeds stay unused |
| **3c. Aspects** | done | one region across categories, aspect sources with decay, `aspectAt`, summary peaks, labels as a list, `parentId` and the containment stage deleted |
| **3d. Tile seeds** | done | tile `placeCategories`, terrain regions, road junction split |
| **3e. Structural qualities** | done | bordering walls feed the aspect field (orthogonal contact, both sides) and appear in facts |
| **3f. Naming verb** | done | `[NAME_PLACE]`, `[PLACES]`, `[MOVE_TO]` resolves remembered places, `Location` and `%LOCATIONS%` deleted |

Acceptance for 3c: a market and an orchard within the link radius are **one** place; its `orchard`
aspect is high at the orchard edge and near zero at the market's far corner; a single stall in a field
is an aspect, not a place; two names attach to the same region as two labels.

## 13. Deliberately not built

- Persistence. Names die with the session; seeds survive.
- Instance-state aspects. Template values only until a condition component exists.
- Time decay. Ages are reported, not modelled.
- Deeper structure of any kind: no children, at any depth.
- Auto-labelling. The engine never writes a label.

## 14. Open questions

1. **Decay shape.** Linear is the default. A gaussian would be smoother and a step function would be
   crisper; none has been tested against a player's reading of the map.
2. **Link chaining.** One dense row can chain a whole district into one place. Raise the link radius,
   or cap a place's member count?
3. ~~Peak or mean for the primary aspect?~~ Resolved: mean, because peak picks one seed. Both are
   still shown; the digest will want both.
4. **Place budget.** 24 places, now that one place can hold many aspects and there are no children.
5. **Where do `places.json` and template categories live once data packs exist?** Inside the data pack
   (`docs/asset-generation-design.md` §9.3).

## 15. References

- `docs/spatial-memory-design.md` — cells, shapes, caps, the place rule being replaced
- `include/SpatialMemory.hpp` — `ObserveObjects`, `DerivePlaces`, `AspectAt`, `ToShapes`, `DumpJson`
- `include/PlaceCategoryRegistry.h` — the taxonomy and its file format
- `include/Components.h` — `SeenObject`, `KnownPlace`, `AspectSource`, `PlaceAspect`, `Structural`
- `src/GameECS.cpp` — observation call site, name-seed loading, `HarvestResolutionSystem`
- `data/places.json`, `data/objects/*.json`, `data/tiles/*.json` — the vocabulary
