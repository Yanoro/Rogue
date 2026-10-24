# Perceived regions and semantic places

**Status:** design, 2026-10-23. **Stages 1–5 implemented 2026-10-23:** open-ground local compass,
direction-first compass, base regions, region categorization, the 16/32 LOD + model-facing region
view, and the semantic-overlay polish. The semantic place derivation (`Memory::DerivePlaces`), the
S1 compass/candidates and the per-call navigation graph visual are implemented. Remaining work is
tuning the exact split/merge/eviction edge cases, the surface vocabulary and performance.

**Date:** 2026-10-23

**Related:** `docs/place-derivation-design.md` (the existing semantic place rule),
`docs/spatial-memory-design.md` (observed cells, objects, aspects and the acquisition path),
`docs/llm-s1-navigation-design.md` (the strategist observation, the S1 pilot and the Navigation
window), `docs/navigation-subagent-design.md` (the S1 compass and candidates),
`docs/agent-exploration-task.md` (the fair-test brief), `SETTINGS.md`.

---

## 1. Decisions at a glance

| decision | choice |
|---|---|
| Base layer | Observed **`KnownPlace` regions**, `PlaceOrigin::Region`; columns of observed ground, not semantic places. |
| Semantic layer | The existing **`Memory::DerivePlaces`** places remain unchanged as an overlay: road, orchard, market, village. |
| Underfoot query | `PlaceQuery::At` prefers the semantic place underfoot; otherwise the current base region. |
| Resolution | **Adaptive 8/16/32 LOD**: 8×8 near, 16×16 mid-distance, 32×32 far (quadtree-equivalent); the exact split/merge is an open question. |
| Region character | The existing **`PlaceAspect`/`PlaceFact`/`PlaceCategoryRegistry`** field: primary category plus a top-aspect mix ("sand 82%, grass 10%"). |
| Stage 1 (implemented) | `BuildCandidates` builds `here:<direction>` runs over observed walkable cells when no place is underfoot; no provisional `KnownPlace`, no region node, no `[PLACES]` entry. |
| Stage 1.5 (implemented) | One best move per sector regardless of `PlaceQuery::At`: current place run → adjacent place → open-ground run. Direction-aware cap: nearest adjacent reserved, then cardinals first, diagonals after. |
| Stage 2 (implemented) | `SpatialMemory::regions`; `DeriveRegions` for 8×8 cells within 12 tiles; all observed cells; facts; separate `maxRegions` 64; `PlaceQuery` combines both layers; regions nameable. |
| Stage 3 (implemented) | Surface sources in `BuildAspects`; the region primary is the dominant surface; `regionLabel` in `data/places.json` (`grass field`, `sand field`, ...); objects inside the region contribute aspects; the semantic place keeps its own forming primary. |
| Stage 4 (implemented) | 8/16/32 parent/child views (`regionLevel`); near 12 / mid 24; `PlaceQuery::ModelPlaces` (semantic + current + nearest `modelNearCount`); a coarser parent is hidden while a finer selected child is contained; `maxRegions` eviction prioritizes current, named, semantic, near, then distance. |
| Stage 5 (implemented) | `At` prefers a semantic place underfoot even when it is larger than the region; `RegionAt` exposes the current region; `[PLACES]` lists semantic places first, adds `#id` hints for duplicate display names, and is model-filtered; a named parent passes its label to the child containing its anchor; the graph labels regions R8/R16/R32. |
| Region fields | `SeedKind::Region`; a region claims only its observed `memberCells`; labels stay empty until named. |
| Ordering | Semantic places first; the current region is always included; the model-facing caps stay unchanged. |
| Region creation | Partial observed regions; unobserved corners are never claimed. |
| Model-facing | The S1 `SURROUNDINGS`/`ACTIONS`, the strategist graph, `[PLACES]` and the summary show the current/near regions and the semantic places, within the existing candidate/graph caps. |
| Fair test | The engine provides vocabulary and weights only; categories are computed from `SpatialMemory`; no engine preference for terrain. |
| Not a map | Regions are created only from `SpatialMemory`; no authored tile map, no undiscovered cell, no engine biome bias. |
| Not a replacement | The semantic places keep their natural extents, labels, aspects and linear-road rules. |

The distinction in one sentence: **a region is a piece of observed ground; a semantic place is a
piece of ground with a character.** The region layer is the floor; the semantic places are the
furniture.

---

## 2. The problem

### 2.1 The current place layer is evidence-sparse

`Memory::DerivePlaces` starts places from **seeds**: observed objects and tiles whose categories
are marked `formsPlace` in `PlaceCategoryRegistry`. A place is a connected cluster of those seeds
with its own `memberCells`, extents, `facts`, `aspects` and `primaryAspect`.

Open grass has no seed categories. A field, a clearing, a stretch of sand or a broad meadow can be
observed in detail and still produce **no place at all**. The current system works well for roads,
orchards, markets and villages because those are exactly the areas that produce seeds.

### 2.2 What that does to navigation

When the character stands in an open field:

- `PlaceQuery::At` returns `nullptr`;
- `NavigationPlan::BuildCandidates` has no current place, so it cannot build `here:<direction>` runs;
- the pilot is offered only distant remembered places, not "walk north across this field";
- `NavigationPlan::BuildState` says *"not standing in any place you know"*;
- the strategist's `BuildGraph` has no node for the open ground;
- `[PLACES]` has nothing to name or target.

The S1 can still walk through cells, but it cannot **reason about the ground as ground**. The
strategist sees a world made of isolated landmarks with empty space between them. That is the
"empty spaces" problem, and it is structural: it is not a prompt issue and it is not fixed by a
better model.

**2026-10-23:** Stage 1 added open-ground `here:<direction>` runs when `PlaceQuery::At` is null;
Stage 1.5 removed that gate entirely. Every sector now offers its best local move — the current
place's run, an adjacent place, or an open-ground run — so a place underfoot can no longer hide an
open direction. Stage 2 adds the base region layer: an observed 8×8 field underfoot is now the
current place, appears in `[PLACES]`, the S1 state and the strategist graph, and can be named. Region
categorization (sand/grass/etc.) is still Stage 3.

### 2.3 The categorization gap

The semantic places already carry a character:

- an orchard: `orchard 100%, road 100%`;
- a road: `road 100%`, linear;
- the market: `market 90%, storage 100%`.

The base layer must carry the same kind of character, or the agent gets only "a field" and cannot
distinguish a grass meadow from a desert:

- west: `sand 82%, grass 10%, rock 8%`;
- east: `grass 74%, sand 15%, orchard 11%`.

That distinction is the difference between a generic coordinate and a place the model can reason
about.

---

## 3. The two layers

```
BASE REGIONS (observed ground, adaptive)              SEMANTIC PLACES (evidence clusters)
+--------+--------+--------+                         ########  a road (linear, road 100%)
| field  | field  | road   |                         ( ( ( ) ) ) an orchard (orchard 87%)
|   @    |        |        |                         [ Village ] (market 90%, storage...)
+--------+--------+--------+
| field  | road   | field  |
+--------+--------+--------+
```

### 3.1 Base region

- Created from observed cells only.
- A coarse spatial unit (8×8 near, 16×16 mid, 32×32 far).
- Holds `memberCells`, `facts`, `aspects`, `primaryAspect`, `firstSeenTick`, `lastSeenTick`.
- Has no semantic label by default; `DisplayName` produces "an unnamed sand field" or "an unnamed
  grass field" from the primary aspect.
- Can be named with `[NAME_PLACE #<id> <name>]` and used as a `[MOVE_TO]` target.
- Exists even when there is no object, road or building.

### 3.2 Semantic place

- The existing `Memory::DerivePlaces` output.
- A connected evidence cluster with a natural extent: a road, an orchard, a market, a village.
- Keeps its labels, `primaryAspect`, aspects, `memberCells`, `IsLinearPlace` behaviour and
  `ApproachCell` rules.
- Can span several base regions.
- Wins underfoot when it is the smaller place: standing on a road returns the road, not "the field
  containing a road".

### 3.3 Division of labour

| question | base region | semantic place |
|---|---|---|
| Can the pilot move through this open field? | yes, local compass | no, if no place exists |
| Is this a road to follow? | weak (road 12%) | yes (road 100%, linear) |
| Is this the market/village? | local character only | yes, named and scored |
| Does the strategist have a node for the ground between landmarks? | yes | no |
| Does the pilot get `here:<direction>` in a field? | yes | no |
| Does the model get meaningful names? | fields/regions | roads, orchards, markets, villages |

The two layers answer different questions and should not compete.

---

## 4. Worked examples

### 4.1 The open field

Wren is in the clearing, two tiles from the road.

Current:

```
YOU ARE: not standing in any place you know.
ACTIONS:
  p1 an unnamed orchard, 1 tile north-west
  p2 an unnamed road, 2 tiles south-west
```

Two-layer:

```
YOU ARE: in an unnamed grass field (grass 90%, road 10%).
ACTIONS:
  here:north      follow the field north for 6 tiles
  here:east       follow the field east for 3 tiles
  here:south-west follow the field south-west for 2 tiles
  p2              an unnamed road, 2 tiles south-west
  p1              an unnamed orchard, 1 tile north-west
```

The pilot can move through the field, and the strategist can say "cross the field south-west to the
road" because the field is a node in the graph.

### 4.2 The road crossing a region boundary

A one-tile road cuts through an 8×8 region.

- The base region: `field 88%, road 12%`; display name "an unnamed field".
- The semantic road: `road 100%`, linear, its own `memberCells`.
- `PlaceQuery::At` returns the road while the character stands on it.
- The pilot gets `here:east` / `here:west` along the road, not "follow the field".
- The `[PLACES]` list shows both:
  ```
  #12 an unnamed road -- road 100% -- you are here
  #13 an unnamed field -- field 88%, road 12% -- 0 tiles east
  ```
- The graph has both nodes: the road for "follow this", the field for "the ground around it".

This is why the base layer must not replace the semantic layer: a grid-only system would call the
region "a field with 12% road" and lose the road's linear character.

### 4.3 The desert west, the grass east

Two adjacent regions:

```
West: sand 82%, grass 10%, rock 8%          East: grass 74%, sand 15%, orchard 11%
      "an unnamed sand field"                     "an unnamed grass field"
```

The S1 sees:

```
YOU ARE: in an unnamed grass field (grass 74%, sand 15%, orchard 11%).
ACTIONS:
  here:west between the grass and the sand for 8 tiles
  here:east follow the grass east for 5 tiles
  p4        an unnamed orchard, 3 tiles east
```

The strategist can say:

> "Head east through the grass toward the orchard; the sand west has no known water or cover."

That sentence is grounded in the aspects, not in a guess. The point of region categorization is to
make this kind of decision expressible.

### 4.4 The village edge

A village spans several regions.

- The semantic village/market remains one large named place with market/storage/workshop aspects.
- The base regions under it are "village edge", "market approach", "field at the village edge".
- At the edge, `PlaceQuery::At` returns the semantic village if the character is inside its cells;
  otherwise the base region.
- The `[PLACES]` list shows the village/market and the local field, not four competing "Villages".
- The duplicate-name hint from the navigation summary still applies if two semantic places share a
  name.

### 4.5 Naming a field

The base region is unnamed until someone names it:

```
[NAME_PLACE #12 North Field]
[MOVE_TO North Field]
```

The region is then a normal `KnownPlace` with a label, and the `[PLACES]` list, the navigation
candidates and the graph use it like any other place.

---

## 5. Data model

### 5.1 Regions as `KnownPlace`

The base region is represented as `KnownPlace` with a new origin and a separate storage vector:

```cpp
enum class PlaceOrigin { Origin, Inferred, Authored, Region };
enum class SeedKind { Object, Tile, Named, Region };
std::vector<KnownPlace> regions;  // beside `places`, not inside it
```

It uses the existing fields:

- `id` — stable for the session, like every `KnownPlace`.
- `origin = PlaceOrigin::Region`.
- `source = KnowledgeSource::Observed`.
- `confidence = 255` (fully observed).
- `anchor`, `width`, `height` — the observed extent of the grid cell.
- `memberCells` — only the observed cells in the region, not the unobserved corners.
- `facts` — surface and object counts (`grass`, `sand`, `road`, `apple_tree`, ...).
- `aspects` — the same `PlaceAspect` summaries as semantic places.
- `aspectSources` — the observed cells/objects and their weights.
- `primaryAspect` — the dominant aspect.
- `labels` — empty until named.
- `firstSeenTick`, `lastSeenTick`.
- `memberSeeds` — empty; a region is not formed by a seed.
- `seedKind` — not meaningful for a region. Either set `SeedKind::Named` as a neutral value or
  add `SeedKind::Region`; the field is not read by navigation.

A region is not a semantic place: it has no `formsPlace`/`minSeeds` rule and no seed cluster. It is a
container of observed cells with a character.

### 5.2 The semantic overlay

`Memory::DerivePlaces` stays as it is. The semantic places keep:

- `origin = Inferred`/`Authored`/`Origin`;
- their `memberCells`, labels, aspects, `primaryAspect`;
- the linear-place behaviour (`IsLinearPlace`, `ApproachCell`);
- the `PlaceCategoryRegistry` `formsPlace`/`minSeeds` rules.

The overlay is not copied into the region; it overlaps it.

### 5.3 Underfoot and sorting

- `PlaceQuery::At` prefers a **semantic place underfoot even when it is larger than the region**:
  inside a village, the village is current, not the smaller 8×8 region. Among semantic places the
  smallest extent wins. A semantic road underfoot still wins over the region.
- If no semantic place contains the cell, `RegionAt` returns the smallest region underfoot.
- `ModelPlaces` always includes the current region (from `RegionAt`) even when the current place is
  semantic, so the local ground remains available.
- `PlaceQuery::SortedPlaces` returns both, sorted by distance to their centres, with semantic
  places and regions distinguishable by `origin`.
- `PlaceQuery::DisplayName` uses the semantic labels first; a region uses
  `"an unnamed " + primaryAspect`, where Stage 3 stores the surface category's `regionLabel`
  (`sand field`, `grass field`, `water`, ...). A region with no recognized surface falls back to
  `primaryAspect = "open ground"`.

### 5.4 Region key and identity

The region key is:

```
level = near | mid | far
size  = 8 | 16 | 32
rx    = floor(x / size)
ry    = floor(y / size)
key   = (level, rx, ry)
```

The key is stable for the session. The `KnownPlace.id` is allocated once and never reused. The
`[PLACES]` number, `[NAME_PLACE]` and the graph use the id; the graph uses `region:<level>:<rx>:<ry>`
for the visual.

---

## 6. Adaptive 8/16/32 LOD

### 6.1 Why not one fixed N

With the current 22-tile circular perception:

| region size | approximate count in the observed disc |
|---|---|
| 1×1 | ~1,500 |
| 4×4 | ~95 |
| 8×8 | ~24 |
| 16×16 | ~6 |

The existing caps are `DEFAULT_MEMORY_PLACES_MAX = 24`, `DEFAULT_NAVIGATION_MAX_CANDIDATES = 8`,
`DEFAULT_NAVIGATION_MAX_DIRECTIONS = 4`, `DEFAULT_NAVIGATION_MAX_GRAPH_NODES = 24`, and
`DEFAULT_NAVIGATION_NEARBY_PLACES = 5`. A single 1×1 or 4×4 grid is a tile map, not a place memory.
The LOD keeps the count bounded while preserving local detail.

### 6.2 Levels

| level | size | where | purpose |
|---|---|---|---|
| near | 8×8 | within ~12 tiles of the character | local compass, field/sand/grass detail |
| mid | 16×16 | ~12–24 tiles | approach/route context |
| far | 32×32 | beyond ~24 tiles | frontier and distant terrain |

The radii are settings; the current defaults are 12 and 24. Observed cells are added to the levels
their distance allows: a near cell appears at 8, 16 and 32; a mid cell at 16 and 32; a far cell at
32. `PlaceQuery::At` returns the finest region underfoot.

### 6.3 Quadtree equivalence

The 8/16/32 levels are a quadtree with three levels. A 32×32 parent contains four 16×16 children,
each contains four 8×8 children. Stage 4 implements the pyramid as **overlapping parent views**:

- every observed cell contributes to all levels its distance allows;
- the 8×8 region is the finest child, the 16×16 and 32×32 regions are its parent views;
- `PlaceQuery::At` picks the finest region underfoot;
- `PlaceQuery::ModelPlaces` hides a coarser parent when a finer selected child is contained in it;
- the parent view remains available for the far/overview picture.

This is a parent-view approximation of the lazy split: the parent is created with the ground rather
than waiting for a separate split pass, and the model-facing view decides when the child or the
parent is shown. The exact split trigger, label inheritance and merge/eviction rules remain open
(Section 14). The invariant is that a region's `memberCells` are observed cells and its aspects are
the aggregate of those cells.

### 6.4 Region creation

Stage 4 runs `Memory::DeriveRegions` after `DerivePlaces` in the same perception sample. All
observed cells are added to the levels their distance allows; regions outside the current bands stay
remembered. The pass is:

1. For each observed cell, add it to each level its distance allows; compute the level key.
2. Get or create the region `KnownPlace` for that key.
3. Insert the cell into `memberCells`; update the observed extent.
4. Accumulate objects and `facts`; build the surface/object aspects and the primary.
5. Refresh `firstSeenTick`, `lastSeenTick`.
6. Enforce `maxRegions`: current first, named regions, regions on semantic ground, near before far,
   then distance and recency.

The pass is incremental: a region is updated only by cells the agent actually observed. Unobserved
corners of the cell are never claimed. A near cell's 16/32 parents include the near cells, which is
what makes the parent view an aggregate rather than a separate claim.

### 6.5 Region count and caps

The model-facing caps are not the same as the storage caps. The storage can hold more regions than
the S1/strategist see. The proposed split:

- **Storage:** a bounded `maxRegions` (for example 48–64), with LOD/eviction for far regions.
- **S1/strategist view:** the current region, the adjacent near regions and the nearest semantic
  places, within `maxCandidates` and `maxGraphNodes`.
- **`[PLACES]`:** the semantic places plus the current/near regions; the existing nearby cap.

The exact `maxRegions` and the region eviction policy are open questions.

---

## 7. Categorization and aspects

### 7.1 Reuse the existing aspect field

The region uses the same machinery as a semantic place:

- `MemoryCell.surface` — grass, sand, path, tilled soil, water;
- `MemoryCell.categories` — tile categories from the definition;
- `SeenObject.categories` — tree, orchard, road, building, market, workshop, sign;
- `PlaceCategoryRegistry` — the category definitions and weights;
- `PlaceFact` — counts for `surface` and `object` facts;
- `PlaceAspect` — peak and mean per category;
- `AspectSource` — the observed sources and their weights.

The region aggregates **all observed categories**, not just `formsPlace` categories. The semantic
`DerivePlaces` pass keeps the `formsPlace`/`minSeeds` rule for creating semantic places.

### 7.2 Primary and mix

Stage 3 populates the region's `aspects`/`aspectSources` from the observed surfaces and objects. The
primary is the **dominant surface** (the tile name), not an object inside the region; the surface
category's `regionLabel` (`grass field`, `sand field`, ...) becomes the display name. The objects
inside the region remain aspects, so a sand region can read `sand 82%, orchard 20%`. A region exposes:

- `primaryAspect` — the dominant category, used for the display name and the graph color;
- the **top 2–3 aspects** with their means — the mix the model sees;
- the full `facts`/counts — the debug view and the detailed tooltip.

Example region:

```json
{
  "primaryAspect": "sand",
  "aspects": {"sand": 0.82, "grass": 0.10, "rock": 0.08}
}
```

The display name is "an unnamed sand field"; the model-facing line keeps the mix so a
sand/grass boundary is not over-smoothed.

### 7.3 Where the categories appear

| view | content |
|---|---|
| `[PLACES]` | `#12 an unnamed sand field -- sand 82%, grass 10%, rock 8% -- you are here` |
| S1 `SURROUNDINGS` | `here: an unnamed sand field (sand 82%, grass 10%, rock 8%)` |
| S1 `ACTIONS` | `here:west follow the sand west for 8 tiles` |
| Strategist graph node | `category: "sand"`, `aspects: {sand:0.82, grass:0.10, rock:0.08}`, `primaryAspect: "sand"` |
| Summary | `You are in an unnamed sand field (sand 82%, grass 10%, rock 8%).` |
| Graph visual | node colored by `primaryAspect`; aspects in the label/tooltip |
| Navigation record | the region id, primary and aspects in the S1 state |

### 7.4 Category vocabulary and the fair test

The category vocabulary is data, not a map:

- the tile definitions provide surfaces/categories;
- the object templates provide categories;
- `PlaceCategoryRegistry` provides the category names, weights, `formsPlace` and `minSeeds`.

The engine may provide the vocabulary (`sand`, `grass`, `water`, `road`); it must not reveal where
sand or grass is. The region's categories are computed from `SpatialMemory`.

The engine should not hard-code a preference ("avoid sand", "prefer grass"). The model can use its
world knowledge; the S1/strategist choose; the pathfinder still respects walkability. This is the
same fair-test rule as the semantic places: observed only, no authored map, no hidden preference.

### 7.5 The semantic category still wins underfoot

Standing on a road:

- the region says `sand 70%, road 30%`;
- the semantic road says `road 100%`, linear;
- `PlaceQuery::At` returns the road;
- the pilot gets `here:east`/`here:west`, not "follow the sand";
- the `[PLACES]` list shows the road and the sand field as separate entries.

The semantic place is the landmark; the region is the ground.

---

## 8. Queries and navigation

### 8.1 Building candidates

The current place is a normal `KnownPlace`: `PlaceQuery::At` returns the semantic place underfoot
(the smallest semantic extent) or the finest region (the 8×8 child) when no semantic place covers
the cell. `NavigationPlan::BuildCandidates` builds one best move per compass sector, regardless of
whether `PlaceQuery::At` finds a place:

1. **Current place run** — if the place underfoot continues along the sector for at least 2 tiles,
   use it (`follow the road east`, `walk north through the orchard`).
2. **Adjacent place** — if the first place in the sector is a different place, use its nearest cell
   (`an unnamed orchard, 2 tiles north`).
3. **Open-ground run** — otherwise walk observed, walkable, unoccupied cells, stopping before a
   different semantic place, an unobserved cell or a blocked one. The target is the last open cell.
   If the current place is only one cell thick in that sector, the open run may cross the current
   place's own cells and continue into the open beyond it; a one-tile sidestep no longer drops the
   direction.

The candidate is `here:<direction>`. A place move keeps the place id and its name/aspects; an
open-ground move has `placeId 0`, label `open ground (...)` and description `walk ... through open
ground ...`. No provisional `KnownPlace`, region node or `[PLACES]` entry is created.

The local cap is direction-aware:

- one slot is reserved for the **nearest adjacent place** (the existing "stay or leave into that
  orchard" rule);
- the remaining slots are filled **cardinal-first** (north, east, south, west, then the four
  diagonals), not by run length;
- `maxDirections` and `maxCandidates` still bound the total, so a cap of 4 gives the nearest
  adjacent place plus three cardinals, while a cap of 8 can show the whole compass. A short north run
  is no longer dropped because the south runs are longer.

The S1 state and summary say "in open ground" when nothing is underfoot; the `arrived` sentinel says
"not at a known place". The target remains a cell, so the existing `MoveToCellAction` and exact-arrival
rules are unchanged.

With the base region layer (Stage 2), the current region replaces the ad-hoc open-ground area:

1. Resolve `current = PlaceQuery::At(memory, here)`: a semantic place or a region.
2. Build the local compass from the current place's observed cells — the same `here:<direction>`
   runs, whether the current is a semantic road or a region field. The open-ground fallback stays
   for the case where the current cell is not yet in any region.
3. Add the adjacent semantic places/regions and the distant places within the existing
   `maxDirections`, `maxCandidates` and `compassRadius`. The distant list uses
   `PlaceQuery::ModelPlaces`, so the parent views do not crowd out the near regions.
4. Resolve every candidate to a target cell as today, so a hallucinated name still cannot move the
   character.

The local compass is the key win: a current region gives the pilot `here:north` in open ground.

### 8.2 State and pilot prompt

`NavigationPlan::BuildState` and `NavigationStrategist::BuildPilotObservation` add the current
region's character:

```
YOU ARE: in an unnamed sand field (sand 82%, grass 10%, rock 8%).
...
SURROUNDINGS:
  here: an unnamed sand field (sand 82%, grass 10%, rock 8%)
  west: sand, 8 tiles
  east: grass, 5 tiles; an unnamed orchard, 3 tiles
ACTIONS:
  here:west follow the sand west for 8 tiles
  here:east follow the grass east for 5 tiles
  p4 an unnamed orchard, 3 tiles east
```

Stage 1 already changes the state and summary to "open ground". The region layer adds the current
region's character (name and aspects) on top of that, so the field reads "an unnamed sand field"
rather than "open ground".

### 8.3 Strategist observation and graph

`NavigationStrategist::BuildObservation` / `BuildGraph` include both node kinds:

- `region` nodes with `anchor`, `memberCells`/extent, `category`, `aspects`, `primaryAspect`,
  `visited`;
- `place` nodes (semantic) with their labels, aspects and extents;
- `object` nodes as today;
- edges: region→region adjacency/traversal, region→semantic overlap, semantic→semantic adjacency,
  object visibility.

The frontier includes the observed edges of the current region and the nearest semantic places.
The graph is built per strategist call and stored with the call snapshot, so the Navigation window's
graph view shows the region picture at that decision point.

### 8.4 Summary

`NavigationPlan::BuildSummary` uses the current region's display name and aspects, and the nearby
list includes the semantic places and the nearest regions. The summary must not dump every region;
the existing nearby cap and the summary's `Journey:` line keep it readable.

### 8.5 Naming and `[MOVE_TO]`

A region is a normal `KnownPlace` for naming and movement:

```
[NAME_PLACE #12 The Dunes]
[MOVE_TO The Dunes]
```

The `[PLACES]` list can include the current/near regions; the `[MOVE_TO]` and `[NAME_PLACE]` rules
are unchanged. A region with no label is addressed by its `[PLACES]` number only through
`[NAME_PLACE]`, as today.

---

## 9. Model-facing selection and caps

The storage layer can hold many regions; the model must not see all of them.

- **S1:** the local compass still uses the finest current region (`PlaceQuery::At`); the distant
  candidates, `[PLACES]`, the summary, the S1 surroundings and the main-model place context use
  `PlaceQuery::ModelPlaces`: all semantic places, the current region, and the nearest
  `modelNearCount` regions.
- **Parent views:** a coarser region is omitted when a finer selected region is contained in it.
  The near 8×8 child wins; its 16×16/32×32 parents are the far/overview picture, not additional
  nearby places.
- **Strategist graph:** the current region, the candidate regions/places, nearby objects; the
  existing `maxGraphNodes`/`maxGraphEdges`.
- **`[PLACES]`:** the same `ModelPlaces` set in model order — semantic places first, then the
  current region and nearest regions. Duplicate display names get an explicit `#id` line and a
  `[NAME_PLACE #id <name>]` hint. The summary nearby list uses
  `DEFAULT_NAVIGATION_NEARBY_PLACES`.
- **Summary:** current region, nearby semantic places, the selected regions; no full region dump.

The principle is: **semantic places are landmarks and should not be crowded out by generic
regions; the current region is always present; the parent view is context, not a second place.**

---

## 10. Fair test

The base layer is inside the same fair-test boundary as every other place:

- regions are created only from cells/objects in `SpatialMemory`;
- no authored map, `Location` rectangle, or undiscovered cell contributes to a region;
- the category vocabulary and weights are data;
- the engine does not reveal where a category is or prefer one;
- the model may use its own world knowledge; the S1/strategist decide;
- the `MoveToCellAction` still respects walkability and line of sight for perception.

The `PlaceNameSeed`/authored names still attach to observed places as today; a region can be named
by the agent with `[NAME_PLACE]`.

---

## 11. Configuration

The existing `perception` section controls the vision field. The `regions` section controls the
base layer:

```json
"regions": {
  "enabled": true,
  "nearSize": 8,
  "midSize": 16,
  "farSize": 32,
  "nearRadius": 12,
  "midRadius": 24,
  "maxRegions": 64,
  "topAspects": 3,
  "modelNearCount": 6
}
```

- `nearSize`/`midSize`/`farSize` — the LOD cell sizes.
- `nearRadius`/`midRadius` — the distance where the level changes.
- `maxRegions` — the storage cap, not the model-facing cap.
- `topAspects` — how many aspects the prompts/graph show.
- `modelNearCount` — how many nearby regions the S1/strategist/`[PLACES]` may see; the current
  region is always included.

The implemented values are the defaults above. The `MAX_PLACES`/`MAX_CANDIDATES`/graph caps stay
small; the storage cap is separate.

---

## 12. Performance and records

- The region pass is incremental: only newly observed cells update a region; the aspects are
  recomputed from the aggregate.
- The 8×8 near level is cheap; the 16/32 levels are aggregates, not per-cell work.
- The region count is bounded by the LOD and `maxRegions`.
- The per-call graph snapshot grows because it includes region nodes; the existing
  `maxGraphNodes`/`maxGraphEdges` and the call history cap keep the debug state bounded.

The Navigation window's **Agent location graph** already draws the per-call graph; region nodes
should use the primary terrain as their color and the top aspects in the label/tooltip. The existing
S1 state and summary show the region's character.

Useful metrics:

- arrivals, legs, surveys, repeats;
- empty-current decisions (the "not standing in a place" branch) — should fall;
- region count per episode, near/mid/far;
- candidate coverage (does the S1 have a local move in a field?);
- prompt/graph tokens;
- per-call graph changes;
- fair-test audits.

---

## 13. Stages

### Stage 1 — local compass in open ground (implemented 2026-10-23)

`BuildCandidates` builds `here:<direction>` runs from the current cell and nearby observed walkable
cells when `PlaceQuery::At` is null. The run stops before a semantic place, an unobserved cell or a
blocked cell; the target is the last open cell. No provisional `KnownPlace`, region node or
`[PLACES]` entry is created. The S1 state and summary say "open ground"; the `arrived` sentinel says
"not at a known place". The harness checks `here:north`/`here:east` targets, the state, the summary
and the arrived wording; the full build passes.

### Stage 1.5 — direction-first compass (implemented 2026-10-23)

The null-only rule was too narrow: a road running east-west could suppress the open north direction,
and a cap sorted by run length could drop a short but needed direction. `BuildCandidates` now builds
one best move per sector in all cases:

- current place run, else adjacent place, else open-ground run;
- a one-cell-thick current place is crossed by the open run instead of being lost to the 2-tile
  minimum;
- the nearest adjacent place keeps one reserved slot;
- the rest of the cap is cardinal-first, then diagonals;
- every move is still a fixed target cell, and a `here:` move is never a provisional place.

The harness adds the road-underfoot/open-north/south-place case: `here:north` must exist, the south
place must remain, and the S1 state/pilot observation must not call the road a dead end while open
ground continues.

This is still the baseline before regions exist: it makes open directions first-class but does not
give the strategist a field graph or `[PLACES]` a field to name. Stage 2 adds those.

### Stage 2 — base regions, 8×8 near (implemented 2026-10-23)

- `SpatialMemory::regions`; `PlaceOrigin::Region`; `SeedKind::Region`; `Memory::DeriveRegions`.
- 8×8 cells within `nearRadius` 12; all observed cells (walls/water included); facts built;
  `primaryAspect = "open ground"`; aspects empty until Stage 3; separate `maxRegions` 64.
- `PlaceQuery::AllPlaces`; `At` prefers the smaller semantic place, otherwise the region;
  `SortedPlaces`/`ById`/`ByLabel` search both.
- `BuildCandidates`, `BuildState`, `BuildGraph`, `BuildSummary`, `SightReport`, `[PLACES]` and the
  main-model place context include regions.
- Regions can be named with `[NAME_PLACE]`; ids and labels survive `DeriveRegions`; `[MOVE_TO]`
  resolves them after naming.
- Tests: open field region, region facts/cells, region in the sorted list and local compass, naming
  and label preservation, semantic place underfoot winning over the region.

Not in Stage 2: terrain aspects/categories (Stage 3), 16/32 LOD and model-facing region-count
selection (Stage 4), and any region-specific model cap changes.

### Stage 3 — region categorization (implemented 2026-10-23)

- `BuildAspects` adds one source per observed **surface** (the tile name) using the category data;
  the semantic place keeps its forming primary, and its aspects now include the terrain.
- `data/places.json` gained `grass`, `sand`, `marsh`, `rock`, `path`, `tilled soil` and
  `packed earth` with `regionLabel`; `water` gained `regionLabel: "water"`.
- `DeriveRegions` collects non-portable, non-structural objects inside the region into
  `memberSeeds`, so object categories become region aspects; walls remain the contact rule.
- The region primary is the dominant surface by mean; the display is the surface's `regionLabel`
  (`an unnamed sand field`, `an unnamed grass field`). With no recognized surface it stays
  `open ground`.
- The S1 state/`SURROUNDINGS`, `[PLACES]`, strategist graph, summary and current-region candidate
  description all show the surface aspects.
- Tests: the open field becomes a grass field; a sand-west/grass-east boundary produces the two
  primaries and aspects; the semantic road underfoot still wins.

### Stage 4 — adaptive LOD and model-facing region view (implemented 2026-10-23)

- `KnownPlace.regionLevel` (0 near 8×8, 1 mid 16×16, 2 far 32×32).
- `DeriveRegions` adds every observed cell to the levels its distance allows: near cells at 8/16/32,
  mid cells at 16/32, far cells at 32. The 16/32 regions are parent views of the same ground.
- `regions.midSize` 16, `regions.midRadius` 24, `regions.farSize` 32; `nearRadius` 12.
- `PlaceQuery::ModelPlaces` returns all semantic places, the current region, and the nearest
  `modelNearCount` regions; a coarser parent is hidden while a finer selected child is contained in
  it. `BuildCandidates`'s distant list, `[PLACES]`, the summary, the S1 surroundings and the
  main-model place context use this view.
- `maxRegions` eviction: current, named, semantic ground, near→far, distance, recency.
- `regions.modelNearCount` is wired to `BuildCandidates`, `[PLACES]` and the summary; the current
  model caps (5 nearby, 8 candidates, 24 graph nodes) remain.
- Tests: near/mid/far levels, parent hiding, `maxRegions` eviction with the current region
  surviving, and the existing sand/grass + semantic overlay tests.

### Stage 5 — semantic overlay polish (implemented 2026-10-23)

- `PlaceQuery::At` now prefers a semantic place underfoot even when the region is smaller (the
  village/road case); `RegionAt` keeps the current region available; `ModelPlaces` always includes
  it. `IsLinearPlace`/`ApproachCell` remain semantic-place behaviour; a region is never linear.
- `[PLACES]` lists semantic places first, then the current/near regions; duplicate display names
  get an explicit `#id` line and a `[NAME_PLACE #id <name>]` hint; the list is model-filtered.
- A named parent region passes its label to the child containing the parent's anchor; the other
  children stay unnamed, and the parent view keeps its own label. This makes `[MOVE_TO]` keep
  resolving to the detailed child under the name.
- The strategist graph node carries `regionLevel`; the Navigation window labels region nodes
  `[R8]`/`[R16]`/`[R32]`, so parent and child are visible in the per-call graph. Region nodes use
  their grid-cell centre (`regionCell` + `regionSize`), so their position does not move as the
  observed extent grows; the current region is included even when a semantic place is current. All
  call graphs share one world-to-screen frame.
- Fair-test audit: every region `memberCell` is an observed cell and every `aspectSource` is on a
  claimed cell; the harness asserts this.
- The docs and the `[PLACES]`/graph tests are updated.

Acceptance for the whole feature:

- An open field is navigable with local compass moves and is represented in `[PLACES]`, the S1
  state and the strategist graph.
- A road underfoot still behaves as a linear semantic place.
- A sand/grass boundary is visible and distinguishable in the S1, strategist and summary.
- The observation contains no unobserved cell, map seed or undiscovered place.
- The region count stays within the model-facing caps; the full graph remains readable.

---

## 14. Decisions taken (2026-10-23) and remaining details

All ten open questions from the first draft were answered; the choices below are now part of the
design.

| # | decision |
|---|---|
| 1 | **Lazy split + parent view.** Create the 8×8 regions and 16/32 parents lazily; split when the character approaches or a semantic place needs detail; keep the children and aggregate a parent view on retreat. Near radius 12, mid radius 24, `maxRegions` 64. |
| 2 | **`SeedKind::Region` + observed subset.** A region claims only its observed `memberCells`; `anchor`/`width`/`height` describe that subset; labels stay empty until named. |
| 3 | **Existing sources + terrain names.** Use the tile surfaces, tile categories and object categories; add terrain names such as sand, grass, water and marsh to the registry; show the primary plus the top 3 aspects. |
| 4 | **Semantic first, current region always in.** Semantic places sort before regions; the current region is always included even if it would fall outside the cap. |
| 5 | **Keep model-facing caps, separate storage cap.** S1 keeps 8 candidates, the graph 24 nodes, the nearby list 5; regions get a larger storage cap; the current region is always included. |
| 6 | **Smallest place; semantic wins.** `PlaceQuery::At` keeps its smallest-`memberCells` rule; a region is never linear. |
| 7 | **Nameable, stable ids, normal rules.** Regions can be named with `[NAME_PLACE]`; ids and `[PLACES]` numbers stay stable; `[MOVE_TO]` works once named; the duplicate-name hint applies. |
| 8 | **Partial observed region.** Create a region with only observed `memberCells`; unobserved corners are never claimed; first/last seen update; `overBudget` guards apply. |
| 9 | **Temporary local area (implemented).** Stage 1 builds open-ground runs from the current cell and observed walkable cells; no provisional `KnownPlace`. Stage 1.5 makes the compass direction-first in every case. |
| 10 | **Vocabulary only, no engine bias.** The engine provides category vocabulary and weights; categories come from `SpatialMemory`; no preference for sand/grass/water; the model may use its world knowledge; walkability is the only movement rule. |

### Remaining details

1. **Exact lazy split and merge.** The pyramid is an overlapping near/mid/far parent view; the
   label inheritance is implemented for a named parent's anchor child, but the exact split/merge
   trigger and what happens when a named parent is split by a later observation still need testing.
2. **Eviction edge cases.** The implemented priority is current → named → semantic ground →
   near→far → distance → recency. Named parents whose children are unnamed, and a semantic place
   straddling two regions, should be tested on long runs.
3. **Surface vocabulary.** The terrain categories and `regionLabel`s are first-pass data; the exact
   set, weights and display labels may change as the map vocabulary grows.
4. **Fair-test audit.** The regions claim only observed cells/surfaces; the harness checks region
   cells/aspect sources, but a real-map audit should also check that no authored map cell contributes.
5. **Performance.** The near/mid/far region pass, the surface aspect sources, the combined
   `PlaceQuery`/`ModelPlaces` view and the `[PLACES]`/graph snapshots should be measured on the
   22-tile perception radius; the pass runs per perception sample, not per frame.

## 15. References

- `docs/place-derivation-design.md` — the semantic place rule, seeds, aspects, `formsPlace` and
  `minSeeds`.
- `docs/spatial-memory-design.md` — observed cells/objects, the acquisition path and the aspect
  field.
- `docs/llm-s1-navigation-design.md` — the strategist observation, the S1 pilot, the per-call graph
  visual.
- `docs/navigation-subagent-design.md` — the S1 compass, candidates and the current place
  requirement.
- `docs/agent-exploration-task.md` — the fair-test brief and the exploration scenarios.
- `SETTINGS.md` — the `perception` settings and the proposed `regions` section.

---

## 16. Glossary

- **Region** — a coarse observed ground unit in the base layer; no semantic identity by default.
- **Semantic place** — an evidence cluster with a natural extent and character: road, orchard,
  market, village.
- **Aspect** — a category's weight/mean/peak in a place or region (for example `sand 0.82`).
- **Primary aspect** — the dominant category, used for the display name and graph color.
- **LOD** — level of detail: 8×8 near, 16×16 mid, 32×32 far.
- **Frontier** — the observed boundary a strategist can aim at; unvisited regions/places and open
  directions.
- **Observed-only** — created solely from `SpatialMemory`, never from the authored map.
